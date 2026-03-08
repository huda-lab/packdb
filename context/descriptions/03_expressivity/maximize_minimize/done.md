# MAXIMIZE / MINIMIZE — Implemented Features

The `MAXIMIZE` or `MINIMIZE` keyword specifies the **optimization objective** — the quantity the solver should optimize while satisfying all constraints.

---

## Syntax

```sql
MAXIMIZE objective_expression
MINIMIZE objective_expression
```

The objective expression must be a single aggregate expression using `SUM()`.

---

## Requirements

1. Must use `SUM()` — other aggregates are not supported over decision variables (see [sql_functions/todo.md](../sql_functions/todo.md) for planned support).
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

### ABS() in Objective

`ABS(expr)` is supported in objectives via automatic linearization. Each `ABS(expr)` over decision variables is rewritten to an auxiliary variable with two linearization constraints.

```sql
MINIMIZE SUM(ABS(new_hours - hours))    -- minimize total absolute deviation
MINIMIZE SUM(ABS(x - a) + ABS(y - b))  -- multiple ABS terms
```

The rewrite happens before normalization in `bind_select_node.cpp` via `RewriteAbsLinearization()`. Auxiliary variables are hidden from query output. See [sql_functions/done.md](../sql_functions/done.md) for the full linearization details.

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
  - Lines 91-100: Validates that only `SUM` is used (rejects other aggregates with error message)
  - Lines 29-66: Handles WHEN condition extraction on objective

- **Execution** (objective coefficient construction + WHEN masking):
  `src/execution/operator/decide/physical_decide.cpp:263-282`
