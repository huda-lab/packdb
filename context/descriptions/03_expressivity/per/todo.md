# PER Keyword — Planned Features

---

## Row-Varying RHS with PER

**Priority: Low**

```sql
-- NOT YET SUPPORTED
SUM(x * hours) <= max_hours PER empID
```

Where `max_hours` varies per group. Requires resolving which row's value to use per group (e.g., validate all rows in a group have the same value, or take the first). Users can work around this today by using multiple WHEN constraints with explicit values.
