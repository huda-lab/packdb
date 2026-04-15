# AVG Aggregate Test Coverage — Done

Tests live in `test/decide/tests/test_avg.py` (9 tests) plus interactions in
`test_aggregate_local_when.py` and `test_per_objective.py`.

## Scenarios covered

| Scenario | Where | Oracle |
|----------|-------|--------|
| AVG in constraint (`AVG(x) op K`) | `test_avg.py` | ✓ |
| AVG in objective (flat = SUM argmax) | `test_avg.py` | ✓ |
| AVG + WHEN | `test_avg.py` | ✓ |
| AVG + PER (per-group average) | `test_avg.py` | ✓ |
| AVG + WHEN + PER | `test_avg.py` | ✓ |
| AVG with BOOLEAN variables | `test_avg.py` | ✓ |
| AVG with INTEGER variables | `test_avg.py` | ✓ |
| AVG in bilinear constraint | `test_avg.py` | ✓ |
| AVG with no decide variable (passthrough) | `test_avg.py` | — |
| Aggregate-local WHEN on AVG | `test_aggregate_local_when.py` | ✓ |
| Aggregate-local WHEN + AVG + PER | `test_aggregate_local_when.py` | ✓ |
| Nested `SUM(AVG(x * cost)) PER col` with unequal groups | `test_per_objective.py::test_sum_avg_per_unequal_groups` | ✓ |
| Nested `MAX(AVG(...)) PER col` (easy MAX) | `test_per_objective.py::test_minimize_max_avg_per` | ✓ |
| Nested `MIN(AVG(...)) PER col` (easy MIN) | `test_per_objective.py::test_maximize_min_avg_per` | ✓ |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| AVG | BOOLEAN | ✓ |
| AVG | INTEGER | ✓ |
| AVG | WHEN (expression-level) | ✓ |
| AVG | WHEN (aggregate-local) | ✓ |
| AVG | PER | ✓ |
| AVG | WHEN + PER | ✓ |
| AVG (inner) | SUM / MAX / MIN (outer) + PER | ✓ |
| AVG | bilinear constraint | ✓ |
| AVG | entity-scoped | ✓ (`test_entity_scope.py::test_entity_scoped_with_avg`) |
