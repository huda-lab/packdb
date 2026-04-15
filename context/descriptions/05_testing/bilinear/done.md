# Bilinear Term Test Coverage — Done

Tests live in `test/decide/tests/test_bilinear.py` plus interactions in
`test_aggregate_local_when.py` and `test_quadratic_constraints.py`.

Bilinear terms (`x * y`, two different DECIDE variables) split into two
categories:

1. **Boolean × anything**: exact MILP via McCormick envelopes, works with both
   solvers. Bool × Bool uses simpler AND-linearization.
2. **General non-convex** (Real×Real, Int×Int, Int×Real): Q matrix off-diagonal
   entries, Gurobi only (NonConvex=2).

## Scenarios covered

### McCormick (Boolean × anything — both solvers)

| Scenario | Where | Oracle |
|----------|-------|--------|
| Bool × Bool objective (AND-linearization) | `test_bilinear.py::TestBilinearBooleanObjectives::test_bool_times_bool_objective` | ✓ |
| Bool × Real objective | `test_bilinear.py` | ✓ |
| Bool × Int objective | `test_bilinear.py` | ✓ |
| Data coefficient scaling (`profit * b * x`) | `test_bilinear.py` | ✓ |
| Bilinear in Bool × Bool constraints | `test_bilinear.py` | ✓ |
| Bilinear + WHEN | `test_bilinear.py::test_bilinear_with_when` | ✓ |
| Aggregate-local WHEN on bilinear constraint | `test_aggregate_local_when.py::test_bilinear_aggregate_local_when_constraint` | ✓ |
| Aggregate-local WHEN on bilinear objective | `test_aggregate_local_when.py::test_bilinear_aggregate_local_when_objective` | ✓ |
| Backward compatibility (existing linear tests still pass) | `test_bilinear.py` | ✓ |

### Non-convex (Gurobi only)

| Scenario | Where | Oracle |
|----------|-------|--------|
| Real × Real objective | `test_bilinear.py` | ✓ (gurobi-gated) |
| Int × Int objective | `test_bilinear.py` | ✓ (gurobi-gated) |
| Int × Real objective | `test_bilinear.py` | ✓ (gurobi-gated) |
| Mixed linear + bilinear objective | `test_bilinear.py` | ✓ |

### Error cases

| Scenario | Where |
|----------|-------|
| Triple product (`x * y * z`) rejected | `test_bilinear.py` |
| Missing upper bound on non-Boolean in Bool × non-Bool | `test_bilinear.py` |
| HiGHS rejects non-convex bilinear | `test_bilinear.py` (`_expect_gurobi` pattern) |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| Bilinear | Bool × Bool (AND-linearization) | ✓ |
| Bilinear | Bool × Real (McCormick) | ✓ |
| Bilinear | Bool × Int (McCormick) | ✓ |
| Bilinear | Real × Real (Gurobi Q) | ✓ |
| Bilinear | Int × Int (Gurobi Q) | ✓ |
| Bilinear | Int × Real (Gurobi Q) | ✓ |
| Bilinear | WHEN (expression-level) | ✓ |
| Bilinear | WHEN (aggregate-local) | ✓ |
| Bilinear | MAXIMIZE objective | ✓ |
| Bilinear | constraint | ✓ (Bool × Bool only) |
| Bilinear | linear terms (mixed) | ✓ |
| Bilinear | QP self-product (mixed) | ✓ |
