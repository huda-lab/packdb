# ABS Linearization Test Coverage — Done

Tests live in:
- `test/decide/tests/test_abs_linearization.py` (9 tests)
- `test/decide/tests/test_per_interactions.py` — ABS in aggregate constraint with PER (per-group ABS aux)

The ABS linearization is performed by `DecideOptimizer::RewriteAbs`. For each
`ABS(expr)` referencing a DECIDE variable, an auxiliary REAL variable `d` is
introduced with two constraints: `d >= expr` and `d >= -expr`.

## Scenarios covered

| Scenario | Where | Oracle |
|----------|-------|--------|
| ABS in objective (basic) | `test_abs_linearization.py` | ✓ |
| ABS in objective with WHEN | `test_abs_linearization.py::test_abs_objective_with_when` | ✓ |
| ABS in objective with PER (PER on separate SUM) | `test_abs_linearization.py::test_abs_objective_with_per` | ✓ |
| ABS in per-row constraint (`ABS(expr) <= K`) | `test_abs_linearization.py` | ✓ |
| ABS in aggregate constraint (`SUM(ABS(expr)) <= K`) | `test_abs_linearization.py` | ✓ |
| ABS in aggregate constraint with WHEN (`SUM(ABS(expr)) <= K WHEN cond`, WHEN-masked aux sum) | `test_abs_linearization.py::test_abs_constraint_aggregate_with_when` | ✓ |
| ABS in aggregate constraint with PER (`SUM(ABS(expr)) <= K PER col`, per-group aux) | `test_per_interactions.py::test_per_abs_aggregate` | ✓ |
| Multiple ABS terms in same expression | `test_abs_linearization.py` | ✓ |
| ABS with no decide variable (passthrough) | `test_abs_linearization.py` | — |
| ABS with mixed BOOLEAN + REAL variables | `test_abs_linearization.py::test_abs_mixed_vars` | ✓ |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| ABS | BOOLEAN | ✓ |
| ABS | INTEGER | ✓ |
| ABS | REAL | ✓ |
| ABS | Multiple variable types | ✓ |
| ABS (objective) | WHEN | ✓ |
| ABS (objective) | PER (on a sibling constraint) | ✓ |
| ABS (per-row constraint) | — | ✓ |
| ABS (aggregate constraint) | — | ✓ |
| ABS (aggregate constraint) | WHEN (auxiliary-variable mask propagation) | ✓ (`test_abs_linearization.py::test_abs_constraint_aggregate_with_when`) |
| ABS (aggregate constraint) | PER (per-group aux partitioning) | ✓ (`test_per_interactions.py`) |
