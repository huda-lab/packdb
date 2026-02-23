# MAXIMIZE / MINIMIZE — Planned Features

## ABS() in Objective

**Priority: High** (needed for repair and imputation tasks)

Minimizing absolute deviations is a core objective pattern for data repair:

```sql
-- NOT YET IMPLEMENTED
MINIMIZE SUM(ABS(new_hours - hours))
```

### Linearization

`ABS(expr)` is non-linear but can be linearized using standard ILP technique:
- Introduce auxiliary REAL variable `d` per row
- Add constraints: `d >= expr` and `d >= -expr`
- Replace `ABS(expr)` with `d` in the objective

### Dependencies

- Requires `IS REAL` variable support (see [decide/todo.md](../decide/todo.md))
- Requires Big-M or auxiliary variable introduction in the optimizer (see [../../04_optimizer/query_rewriting/todo.md](../../04_optimizer/query_rewriting/todo.md))

See [sql_functions/todo.md](../sql_functions/todo.md) for the full linearization spec.

---

## PER on Objective

**Priority: High** (needed for per-group optimization)

```sql
-- NOT YET IMPLEMENTED
MINIMIZE SUM(ABS(new_hours - hours)) PER projectID
```

Generates one objective term per distinct value of the PER column. See [per/todo.md](../per/todo.md) for full PER design.

---

## COUNT / AVG Over Decision Variables

These are tracked as aggregate function extensions. See [sql_functions/todo.md](../sql_functions/todo.md) for linearization approaches and implementation suggestions. Key points:

- **COUNT(x)**: Can be rewritten to `SUM(x)` when `x IS BOOLEAN`
- **AVG(x * col)**: Can be linearized by multiplying both sides by the known row count
