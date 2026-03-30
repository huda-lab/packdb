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
2. Must be **linear** in the decision variables (or convex quadratic — see [problem_types/done.md](../problem_types/done.md)).
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

### Other Aggregates in Objectives

All supported aggregates work in objectives. Each is detailed in [sql_functions/done.md](../sql_functions/done.md):

- **COUNT(x)**: Counts non-zero assignments. BOOLEAN (rewritten to SUM) and INTEGER (Big-M indicator rewrite).
- **AVG(expr)**: Flat AVG becomes SUM (same argmax/argmin). Nested with PER: inner AVG scales coefficients by `1/n_g`.
- **MIN(expr) / MAX(expr)**: Linearized via global auxiliary variable. Easy cases (`MINIMIZE MAX`, `MAXIMIZE MIN`) need only linking constraints; hard cases (`MAXIMIZE MAX`, `MINIMIZE MIN`) require Big-M indicators.
- **ABS(expr)**: Linearized via auxiliary REAL variable with two constraints (`d >= expr`, `d >= -expr`).

### Convex Quadratic Objectives (QP/MIQP)

Convex quadratic objectives are supported via `MINIMIZE SUM(POWER(linear_expr, 2))`. See [problem_types/done.md](../problem_types/done.md) for full details on syntax forms, convexity enforcement, solver support, and mathematical formulation.

```sql
MINIMIZE SUM(POWER(x - target, 2))    -- L2 minimization
```

### WHEN on Objective

The objective can be made conditional: only rows where the `WHEN` condition holds contribute. Non-matching rows have their coefficients zeroed out. See [when/done.md](../when/done.md) for full details.

```sql
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'
```

### PER on Objective — Nested Aggregate Syntax

PER on objectives uses nested aggregate syntax: `OUTER(INNER(expr)) PER col`. See [per/done.md](../per/done.md) for full details on the two-level formulation, easy/hard classification, and WHEN+PER composition.

```sql
MINIMIZE SUM(MAX(x * cost)) PER department     -- minimize sum of per-dept max costs
MAXIMIZE MIN(SUM(x * profit)) PER region       -- maximize the worst-performing region
```

---

## Objective and Solver Behavior

For linear objectives, the objective function is compiled into the `c` vector of the standard ILP formulation:

```
maximize  c^T x
subject to  Ax <= b,  x >= 0,  x integer
```

When a `WHEN` condition is present, coefficients for non-matching rows are set to 0. The solver sees a standard dense objective vector.

For quadratic objectives, additional Q matrix terms are added. See [problem_types/done.md](../problem_types/done.md).

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
| Data repair / imputation (L1) | `MINIMIZE SUM(ABS(new_val - old_val))` |
| Data repair / imputation (L2) | `MINIMIZE SUM(POWER(new_val - old_val, 2))` |

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

- **SUM argument validation**: `src/planner/expression_binder/decide_binder.cpp`
  - `ValidateSumArgumentInternal` validates the expression tree inside SUM()

- **Nested aggregate detection**: `src/planner/binder/query_node/bind_select_node.cpp`
  - Detects `OUTER(INNER(expr)) PER col` pattern and validates flat MIN/MAX + PER is disallowed

- **Solver input**: `src/include/duckdb/packdb/solver_input.hpp`
