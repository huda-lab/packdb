# WHEN Keyword — Planned Features

## Interaction with PER

When `PER` is implemented (see [per/todo.md](../per/todo.md)), `WHEN` and `PER` will combine as follows:

1. `WHEN` filters rows first
2. `PER` groups the filtered rows by column value
3. One constraint is generated per distinct group

```sql
-- NOT YET IMPLEMENTED
SUM(new_hours) <= 30 WHEN title = 'Director' PER empID
-- = one constraint per Director's empID: SUM of their hours <= 30
```

**Execution order**: WHEN (filter) -> PER (partition -> generate constraints per group).

No changes to the WHEN implementation itself are needed — PER will build on top of the existing WHEN infrastructure.
