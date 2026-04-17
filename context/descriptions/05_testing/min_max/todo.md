# MIN/MAX Aggregate Test Coverage — Todo

## Closed

- **MIN/MAX + aggregate-local WHEN (hard direction)** — `test_aggregate_local_when.py::test_aggregate_local_when_with_hard_max` (2026-04-17). Oracle-compared: `MAX(x * value) WHEN active >= 6 AND SUM(x) <= 2`. Only active rows get Big-M indicators (encoded via Gurobi native `add_indicator_constraint`, no hand-picked M). Expected optimum {a, c}, obj = 30 — active row `a` is the only one that can meet the >=6 threshold with boolean `x`, non-active row `c` is unconstrained by the MAX-WHEN and fills the second slot.

## Missing coverage

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
