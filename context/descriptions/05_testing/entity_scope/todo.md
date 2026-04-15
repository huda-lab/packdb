# Entity-Scope Test Coverage — Todo

## Upgrade constraint-only tests to oracle

These tests currently verify constraints but not optimality. Remaining items:

| Test | Gap | Oracle strategy |
|------|-----|-----------------|
| `test_entity_scoped_integer_count` | COUNT(INTEGER) Big-M — no objective check | Hard: oracle needs Big-M indicator vars per nation to model COUNT |
| `test_entity_scoped_mixed_when_per` | All-four interaction — constraint-only | Hard: 1500-customer MIP exceeds HiGHS reliability; oracle finds sub-optimal (74993 vs PackDB 74995). Needs Gurobi or problem size reduction to add oracle. |
| `test_entity_scoped_ne_per` | NE + PER — constraint-only | Oracle: hard; NE with PER needs Big-M per group |

Previously completed (oracle added):
- `test_entity_scoped_with_max` — ✓ oracle verifies MAX easy case eligibility
- `test_entity_scoped_max_hard_case` — ✓ oracle verifies MAX >= K qualifying nation constraint
- `test_entity_scoped_min_easy_case` — ✓ oracle verifies MIN easy case eligibility (all blocked on sf=0.01)
- `test_entity_scoped_avg_per` — ✓ oracle verifies per-region AVG scaling
- `test_entity_scoped_two_tables` — ✓ oracle added; also fixed degenerate SUM(keepR) <= 3 → <= 10
- `test_entity_scoped_when_entity_invisible` — ✓ oracle confirms trivially optimal (all filtered, SUM=0)

## Missing feature combinations

| Gap | Risk | Suggested test |
|-----|------|---------------|
| PER STRICT + entity-scoped | HIGH — STRICT changes WHEN→PER order, currently xfail (parser broken) | `test_entity_scoped_per_strict` — unxfail when parser fixed |
| IS REAL + entity-scoped (in entity_scope file) | LOW — covered in `test_quadratic_constraints.py::test_table_scoped_variables` | Add basic IS REAL test here for completeness |
| ABS linearization + entity-scoped | MEDIUM — ABS rewrite may not handle entity-scoped coefficient assembly | `SUM(ABS(keepN * acctbal - 500)) <= K` style constraint |
| Subquery RHS + entity-scoped + PER | MEDIUM — three-way interaction untested | `SUM(keepN) <= (SELECT ...) PER r_name` |
| JOIN fan-out with entity-scoped | MEDIUM — duplicate rows after join could affect entity key dedup | Test with a many-to-many join producing multiple rows per entity |
| NULL in entity key column | LOW — NULL grouping in VarIndexer could silently create wrong entity | `WHERE n_nationkey IS NOT NULL` should be robust; test without filter |
| entity_scope + QP objective | LOW — covered in `test_quadratic_constraints.py` | Verify POWER(keepN, 2) and keepN * x work with entity-scoped |

## Infrastructure

- [ ] Add `perf_tracker.record()` calls to oracle-verified tests (currently only Test 1 passes `perf_tracker` to record timing).
- [ ] Tests 12–26 (new tests) were written without `perf_tracker` in signature; add it if performance tracking of new tests is wanted.
