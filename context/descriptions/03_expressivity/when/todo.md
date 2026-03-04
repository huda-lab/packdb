# WHEN Keyword — Planned Features

## Interaction with PER — IMPLEMENTED

WHEN + PER composition is now implemented. See [when/done.md](done.md) (section "Interaction with PER") and [per/done.md](../per/done.md) for full documentation.

```sql
SUM(new_hours) <= 30 WHEN title = 'Director' PER empID
-- = one constraint per Director's empID: SUM of their hours <= 30
```

No further WHEN features are currently planned.
