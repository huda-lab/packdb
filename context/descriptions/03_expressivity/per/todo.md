# PER Keyword — Planned Features

---

## Multi-Column PER

```sql
-- NOT YET SUPPORTED
SUM(x) <= 40 PER (empID, department)
```

Requires grammar support for parenthesized column lists (comma conflicts with constraint separator), and multi-column group ID assignment. The `row_group_ids` architecture supports this natively — just compute group IDs from the combination of column values.

---

## PER on Objective (Partition-Solve Semantics)

PER on the objective is accepted by the grammar and binder but currently treated as equivalent to global SUM (no-op). This becomes meaningful when partition-solve is implemented:

```sql
-- Currently treated as global MINIMIZE SUM(...)
-- Future: decompose into independent per-group optimization
MINIMIZE SUM(ABS(new_hours - hours)) PER projectID
```

See [../../04_optimizer/problem_reduction/todo.md](../../04_optimizer/problem_reduction/todo.md) for the partition-solve design.

---

## Row-Varying RHS with PER

```sql
-- NOT YET SUPPORTED
SUM(x * hours) <= max_hours PER empID
```

Where `max_hours` varies per group. Requires resolving which row's value to use per group (e.g., validate all rows in a group have the same value, or take the first).
