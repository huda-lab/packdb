# COUNT Aggregate Test Coverage — Todo

## Missing coverage

### MEDIUM: MINIMIZE COUNT(x INTEGER)

Only `MAXIMIZE COUNT(x)` is tested for INTEGER. The indicator-variable direction matters for minimization — the solver wants to drive `z` values to 0, which then drives `x` values to 0 only if the constraint `z <= x` is correctly oriented.

```sql
-- MINIMIZE COUNT INTEGER
SELECT id, x FROM data
DECIDE x
SUCH THAT SUM(x) >= 20 AND x <= 10
MINIMIZE COUNT(x)
```

### LOW: Upgrade `test_entity_scoped_integer_count` from constraint-only to oracle

`test_entity_scope.py::test_entity_scoped_integer_count` is tagged
`✓ (constraint-only)` in `done.md` — it verifies the per-entity count cap is
respected but does not formulate the equivalent gurobipy ILP. Constraint-only
is a legacy tier per `05_testing/README.md`; this test should be upgraded to
use `compare_solutions` with `add_count_integer_indicators` from
`tests/_oracle_helpers.py` (native Gurobi indicator constraints — no Big-M
mirror).
