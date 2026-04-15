# COUNT Aggregate Test Coverage — Done

Tests live in:
- `test/decide/tests/test_count_rewrite.py` — BOOLEAN variable case (COUNT → SUM)
- `test/decide/tests/test_count_integer.py` — INTEGER variable case (COUNT → Big-M indicator)
- `test/decide/tests/test_error_binder.py` — COUNT on REAL rejection
- `test/decide/tests/test_aggregate_local_when.py` — aggregate-local WHEN interaction

The COUNT rewrite is performed by `DecideOptimizer::RewriteCountToSum`. For
BOOLEAN variables, COUNT → SUM directly. For INTEGER variables, an indicator
`z` is introduced with linking constraints `z <= x` and `x <= M*z` (the latter
generated at execution time when M is known).

## Scenarios covered

| Scenario | Where | Oracle |
|----------|-------|--------|
| COUNT(x BOOLEAN) in constraint | `test_count_rewrite.py` | ✓ |
| COUNT(x BOOLEAN) in objective (MAXIMIZE) | `test_count_rewrite.py` | ✓ |
| COUNT(x BOOLEAN) + WHEN | `test_count_rewrite.py::test_count_with_when` | ✓ |
| COUNT(x BOOLEAN) + PER | `test_count_rewrite.py::test_count_with_per` | ✓ |
| Equivalence: COUNT(x BOOLEAN) == SUM(x) | `test_count_rewrite.py` | ✓ |
| COUNT(x INTEGER) constraint upper bound | `test_count_integer.py` | ✓ |
| COUNT(x INTEGER) constraint lower bound | `test_count_integer.py` | ✓ |
| COUNT(x INTEGER) in objective (MAXIMIZE) | `test_count_integer.py` | ✓ |
| COUNT(x INTEGER) + WHEN | `test_count_integer.py::test_count_integer_with_when` | ✓ |
| COUNT(x INTEGER) indicator dedup (multiple references) | `test_count_integer.py` | ✓ |
| COUNT(x INTEGER) hidden indicator from SELECT * | `test_count_integer.py` | ✓ |
| COUNT(x REAL) rejected | `test_error_binder.py::test_count_real_rejected` | error test |
| Aggregate-local WHEN on COUNT | `test_aggregate_local_when.py::test_aggregate_local_when_with_count` | ✓ |
| COUNT + entity_scope | `test_entity_scope.py::test_entity_scoped_with_count` (BOOLEAN), `::test_entity_scoped_integer_count` (INTEGER) | ✓ / constraint only |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| COUNT (BOOLEAN) | WHEN | ✓ |
| COUNT (BOOLEAN) | PER | ✓ |
| COUNT (INTEGER) | WHEN | ✓ |
| COUNT (BOOLEAN) | entity-scoped | ✓ |
| COUNT (INTEGER) | entity-scoped | ✓ (constraint-only) |
| COUNT | aggregate-local WHEN | ✓ |
