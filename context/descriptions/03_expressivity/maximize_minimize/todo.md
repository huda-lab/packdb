# MAXIMIZE / MINIMIZE — Planned Features

## ~~ABS() in Objective~~ — **DONE**

Moved to [done.md](done.md). Implemented via early rewrite (auxiliary REAL variable linearization) in `bind_select_node.cpp`. See also [sql_functions/done.md](../sql_functions/done.md).

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
