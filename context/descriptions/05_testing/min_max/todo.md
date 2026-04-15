# MIN/MAX Aggregate Test Coverage — Todo

## Missing coverage

### HIGH: Hard MIN/MAX constraints with PER

Only easy-case PER stripping and PER on objectives are tested. Hard constraint cases with PER have zero coverage.

**Risk**: Hard MIN/MAX constraints (`MAX(expr) >= K`, `MIN(expr) <= K`, equality) create Big-M indicator variables and linking constraints (`z <= expr`, `z >= expr - M*(1-y)`, `SUM(y) >= 1`). With PER, these must be created *per group* — one auxiliary `z_g` and one set of indicators per distinct PER value. If indicators are created globally, per-group semantics are lost and the solver sees the wrong problem.

```sql
-- Hard MAX constraint with PER
SELECT id, category, cost, profit, x
FROM items
DECIDE x IS BOOLEAN
SUCH THAT MAX(x * cost) >= 50 PER category
MAXIMIZE SUM(x * profit)

-- Hard MIN constraint with PER
SUCH THAT MIN(x * hours) <= 4 PER empID

-- Equality with PER (both directions)
SUCH THAT MAX(x * cost) = 100 PER category
```

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
