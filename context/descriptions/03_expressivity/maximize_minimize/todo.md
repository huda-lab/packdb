# MAXIMIZE / MINIMIZE — Planned Features

## PER on Objective

**Priority: High** (needed for per-group optimization)

```sql
-- NOT YET IMPLEMENTED
MINIMIZE SUM(ABS(new_hours - hours)) PER projectID
```

Generates one objective term per distinct value of the PER column. See [per/done.md](../per/done.md) for current PER documentation and [per/todo.md](../per/todo.md) for the partition-solve design.
