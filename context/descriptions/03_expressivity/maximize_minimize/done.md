# MAXIMIZE / MINIMIZE — Implemented Features

The `MAXIMIZE` or `MINIMIZE` keyword specifies the **optimization objective** — the quantity the solver should optimize while satisfying all constraints.

---

## Syntax

```sql
MAXIMIZE objective_expression
MINIMIZE objective_expression
```

The objective expression must be a single aggregate expression using one of the supported aggregate functions.

---

## Requirements

1. Must use a supported aggregate: `SUM()`, `COUNT()`, `AVG()`, `MIN()`, or `MAX()`. See [sql_functions/done.md](../sql_functions/done.md) for details on each.
2. Must be **linear** in the decision variables.
3. Must involve at least one decision variable.

---

## Supported Objective Forms

### SUM Over Decision Variables

```sql
MAXIMIZE SUM(x * value)            -- maximize total value of selected items
MINIMIZE SUM(x * cost)             -- minimize total cost
MAXIMIZE SUM(x)                    -- maximize number of selected items
```

### SUM with Multiple Variables

```sql
MAXIMIZE SUM(x * profit + y * bonus)
MINIMIZE SUM(x * direct_cost + y * overhead)
```

### SUM with Column Arithmetic

Decision variables may be multiplied by table columns (treated as constants per row).

```sql
MAXIMIZE SUM(x * (price - cost))   -- x * per-row constant expression
```

### WHEN on Objective

The objective can be made conditional: only rows where the `WHEN` condition holds contribute. Non-matching rows have their coefficients zeroed out.

```sql
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'
MINIMIZE SUM(x * cost) WHEN region = 'US'
```

See [when/done.md](../when/done.md) for full details.

### COUNT() in Objective

`COUNT(x)` counts non-zero assignments. Supported for BOOLEAN (rewritten to SUM) and INTEGER (Big-M indicator rewrite) variables.

```sql
MAXIMIZE COUNT(x)                    -- maximize number of non-zero rows
MINIMIZE COUNT(x)                    -- minimize number of non-zero rows
```

### AVG() in Objective

**Flat (no PER)**: `AVG(expr)` in the objective simply becomes `SUM(expr)` — same argmax/argmin since the row count N > 0 is constant.

```sql
MAXIMIZE AVG(x * profit)             -- same as MAXIMIZE SUM(x * profit)
```

**Nested with PER (inner AVG)**: `OUTER(AVG(expr)) PER col` is supported for all outer aggregates (SUM, MIN, MAX, AVG). Unlike flat AVG, inner AVG is NOT equivalent to SUM when groups have different sizes — each group's contribution is `SUM(expr_g)/n_g`, weighting smaller groups more heavily per row. The optimizer rewrites inner AVG → SUM and sets `per_inner_was_avg = true`; the physical execution layer divides each row's coefficient by its group size.

```sql
MINIMIZE SUM(AVG(x * cost)) PER department   -- sum of per-dept average costs
MINIMIZE MAX(AVG(x * hours)) PER empID        -- worst per-employee average workload
MAXIMIZE MIN(AVG(x * profit)) PER region     -- best-worst regional average profit
```

**Nested with PER (outer AVG)**: `AVG(INNER(expr)) PER col` is equivalent to `SUM(INNER(expr))` for optimization — dividing by the constant number of groups G doesn't change the optimal solution.

### MIN() / MAX() in Objective

Supported via linearization with a global auxiliary variable. "Easy" cases (`MINIMIZE MAX`, `MAXIMIZE MIN`) need only per-row linking constraints. "Hard" cases (`MAXIMIZE MAX`, `MINIMIZE MIN`) additionally require Big-M binary indicators.

```sql
MINIMIZE MAX(x * cost)               -- easy: global z with z >= expr_i
MAXIMIZE MIN(x * profit)             -- easy: global z with z <= expr_i
MAXIMIZE MAX(x * profit)             -- hard: z + binary indicators
MINIMIZE MIN(x * cost)               -- hard: z + binary indicators
```

See [sql_functions/done.md](../sql_functions/done.md) for the full linearization details.

### ABS() in Objective

`ABS(expr)` is supported in objectives via automatic linearization. Each `ABS(expr)` over decision variables is rewritten to an auxiliary variable with two linearization constraints.

```sql
MINIMIZE SUM(ABS(new_hours - hours))    -- minimize total absolute deviation
MINIMIZE SUM(ABS(x - a) + ABS(y - b))  -- multiple ABS terms
```

The rewrite is performed by `DecideOptimizer::RewriteAbs` in `decide_optimizer.cpp`. Auxiliary variables are hidden from query output. See [sql_functions/done.md](../sql_functions/done.md) for the full linearization details.

### PER on Objective — Nested Aggregate Syntax

PER on objectives is supported with a **nested aggregate** syntax that combines two levels of aggregation:

```sql
OUTER(INNER(expr)) PER col
```

where `OUTER` and `INNER` are each one of `SUM`, `MIN`, `MAX`, or `AVG`. AVG as outer is equivalent to SUM for optimization; AVG as inner scales coefficients by `1/n_g` (meaningful when groups have different sizes).

**Examples**:

```sql
MINIMIZE SUM(MAX(x * cost)) PER department     -- minimize sum of per-dept max costs
MAXIMIZE MIN(SUM(x * profit)) PER region       -- maximize the worst-performing region
MINIMIZE MAX(SUM(x * hours)) PER empID          -- minimize the peak workload
MINIMIZE SUM(AVG(x * cost)) PER department     -- minimize sum of per-dept average costs
```

#### Flat Aggregate + PER Behavior

- **`SUM(expr) PER col`** or **`AVG(expr) PER col`**: Accepted as a no-op (the global sum of per-group sums equals the global sum).
- **`MIN(expr) PER col`** or **`MAX(expr) PER col`** (flat, no nested aggregate): **Error** — ambiguous semantics. Users must use the nested form to specify both the inner (per-group) and outer (across-group) aggregation explicitly.

#### Two-Level ILP Formulation

The nested aggregate PER objective is linearized in two levels:

1. **Inner level (per group)**: For each distinct value of the PER column, compute the inner aggregate. If the inner aggregate is `SUM`, this is a direct sum of per-group coefficients. If the inner aggregate is `AVG`, same as SUM but each row's coefficient is divided by its group size (`1/n_g`). If the inner aggregate is `MIN` or `MAX`, a per-group auxiliary variable is created with linking constraints (using the same easy/hard classification as non-PER MIN/MAX objectives).

2. **Outer level (across groups)**: The outer aggregate combines the per-group results into a single scalar objective. If the outer aggregate is `SUM` (or `AVG`, which maps to `SUM`), the per-group auxiliaries (or sums) are added directly. If the outer aggregate is `MIN` or `MAX`, a global auxiliary variable is created with linking constraints across groups.

The easy/hard classification applies at each level independently:
- **Easy inner**: `MINIMIZE MAX(...)`, `MAXIMIZE MIN(...)` inner — no Big-M at the group level.
- **Hard inner**: `MAXIMIZE MAX(...)`, `MINIMIZE MIN(...)` inner — Big-M indicators per group.
- **Easy outer**: `MINIMIZE MAX(...)`, `MAXIMIZE MIN(...)` outer — no Big-M across groups.
- **Hard outer**: `MAXIMIZE MAX(...)`, `MINIMIZE MIN(...)` outer — Big-M indicators across groups.

#### WHEN + PER Composition

WHEN filters rows before PER groups them, then the nested aggregate applies:

```sql
MINIMIZE MAX(SUM(x * hours)) WHEN active = 1 PER empID
```

#### Tests

See `test/decide/tests/test_per_objective.py` for comprehensive tests of all 9 combinations.

---

## Objective and Solver Behavior

The objective function is compiled into the `c` vector of the standard ILP formulation:

```
maximize  c^T x
subject to  Ax <= b,  x >= 0,  x integer
```

When a `WHEN` condition is present, coefficients for non-matching rows are set to 0. The solver sees a standard dense objective vector.

---

## Use Cases

| Task | Typical Objective |
|---|---|
| Knapsack / subset selection | `MAXIMIZE SUM(x * value)` |
| Minimize items selected | `MINIMIZE SUM(x)` |
| Active learning (minimize uncertainty) | `MINIMIZE SUM(keep * confidence)` |
| Maximize retained data after cleaning | `MAXIMIZE SUM(keep)` |
| Outlier removal | `MAXIMIZE SUM(keep)` |
| Entity resolution / deduplication | `MAXIMIZE SUM(keepS) + SUM(keepP)` |
| Data repair / imputation | `MINIMIZE SUM(ABS(new_val - old_val))` |

---

## Examples

```sql
MAXIMIZE SUM(x * value)
MINIMIZE SUM(x * cost)
MAXIMIZE SUM(x * value) WHEN category = 'electronics'
MAXIMIZE SUM(keepS) + SUM(keepP)
```

---

## Code Pointers

- **Objective binder**: `src/planner/expression_binder/decide_objective_binder.cpp`
  - Validates that only `SUM`, `AVG`, `MIN`, `MAX` are used (rejects other aggregates with error message)
  - Handles WHEN condition extraction on objective
  - Binds nested aggregate PER objectives (inner/outer aggregate detection)

- **Nested aggregate detection**: `src/planner/binder/query_node/bind_select_node.cpp`
  - Detects `OUTER(INNER(expr)) PER col` pattern and validates flat MIN/MAX + PER is disallowed

- **Execution** (objective coefficient construction + WHEN masking + PER auxiliary building):
  `src/execution/operator/decide/physical_decide.cpp`
