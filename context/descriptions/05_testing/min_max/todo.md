# MIN/MAX Aggregate Test Coverage — Todo

## Missing coverage

### MEDIUM: MIN/MAX with aggregate-local WHEN on hard cases

The aggregate-local WHEN test only covers `MAX(x) <= K WHEN ...` (easy case). No test covers the hard direction with an aggregate-local filter: the Big-M indicators and linking constraints must respect the per-term mask.

```sql
-- Aggregate-local WHEN on hard MAX
SELECT id, category, x FROM data
DECIDE x IS INTEGER
SUCH THAT MAX(x * cost) WHEN (category = 'electronics') >= 50
  AND x <= 100
MAXIMIZE SUM(x)
```

### MEDIUM: `MINIMIZE MAX(expr) WHEN cond` / `MAXIMIZE MIN(expr) WHEN cond` flat objective

`test_per_objective.py` covers nested PER objectives like `MINIMIZE SUM(MAX(expr)) WHEN cond PER col` indirectly. The flat (non-PER) hard objective with WHEN is less exercised. Worth a targeted test.

```sql
-- Flat MIN/MAX objective with WHEN
SELECT id, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 2
MAXIMIZE MIN(x * profit) WHEN category = 'priority'
```

## Cross-references

- `PER + hard MIN/MAX` — see also this folder and `per/todo.md`
- `entity_scope + hard MIN/MAX` — covered in `entity_scope/todo.md`
