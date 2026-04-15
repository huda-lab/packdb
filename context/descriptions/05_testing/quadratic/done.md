# Quadratic Programming Test Coverage — Done

Tests live in:
- `test/decide/tests/test_quadratic.py` — QP/MIQP objectives
- `test/decide/tests/test_quadratic_constraints.py` — QCQP (quadratic constraints)

Covers `POWER(expr, 2)` via Q matrix construction. Convex forms run on both
solvers; non-convex and MIQP require Gurobi. Three syntax forms supported:
`POWER(expr, 2)`, `expr ** 2`, `(expr) * (expr)` self-product, plus negated
variants.

## Scenarios covered (objectives)

| Scenario | Where | Oracle |
|----------|-------|--------|
| `MINIMIZE SUM(POWER(x - target, 2))` (convex, Q=PSD) | `test_quadratic.py` | ✓ |
| `MAXIMIZE SUM(-POWER(x - target, 2))` (concave, Q=NSD) | `test_quadratic.py` | ✓ |
| `MAXIMIZE SUM(POWER(x, 2))` (non-convex, Gurobi only) | `test_quadratic.py` | ✓ (gurobi-gated) |
| Syntax: `expr ** 2` form | `test_quadratic.py` | ✓ |
| Syntax: `(expr) * (expr)` self-product | `test_quadratic.py` | ✓ |
| Simple squared variable (`POWER(x, 2)`) | `test_quadratic.py` | ✓ |
| Various coefficient forms | `test_quadratic.py` | ✓ |
| QP + WHEN | `test_quadratic.py::test_qp_with_when` | ✓ |
| QP objective + PER constraint (`SUM(x)>=5 PER grp` with `MINIMIZE SUM(POWER(x-t,2))`) | `test_per_interactions.py::test_qp_objective_per_constraint` | ✓ (analytical) |
| MIQP (integer vars + QP) | `test_quadratic.py::test_qp_maximize_integer` | ✓ (gurobi-gated) |
| QP + multiple variables | `test_quadratic.py::test_qp_multiple_variables` | ✓ |
| TPC-H data QP | `test_quadratic.py` | ✓ |

## Scenarios covered (constraints — QCQP)

| Scenario | Where | Oracle |
|----------|-------|--------|
| `POWER(expr, 2) <= K` per-row | `test_quadratic_constraints.py` | ✓ (gurobi-gated) |
| `SUM(POWER(expr, 2)) <= K` aggregate | `test_quadratic_constraints.py` | ✓ |
| Zero budget (exact match) | `test_quadratic_constraints.py` | ✓ |
| Binding vs non-binding constraints | `test_quadratic_constraints.py` | ✓ |
| Multi-variable inner expression | `test_quadratic_constraints.py::test_multi_variable_inner_expression` | ✓ |
| Negated `-POWER(expr, 2)` | `test_quadratic_constraints.py` | ✓ |
| Scaled `K * POWER(expr, 2)` | `test_quadratic_constraints.py` | ✓ |
| Data-dependent coefficients | `test_quadratic_constraints.py` | ✓ |
| Quadratic constraint + WHEN | `test_quadratic_constraints.py::test_when_filtering` | ✓ |
| Quadratic constraint + PER groups | `test_quadratic_constraints.py::test_per_groups` | ✓ |
| Multiple quadratic constraints per query | `test_quadratic_constraints.py` | ✓ |
| QCQP: quadratic objective + quadratic constraint | `test_quadratic_constraints.py` | ✓ |
| Mixed linear + quadratic constraint | `test_quadratic_constraints.py` | ✓ |
| Table-scoped (entity) + QP | `test_quadratic_constraints.py::test_table_scoped_variables` | ✓ |
| REAL variable + QP constraint | `test_quadratic_constraints.py` | ✓ |
| INTEGER variable + QP constraint | `test_quadratic_constraints.py` | ✓ |
| Bilinear + self-product mixed | `test_quadratic_constraints.py::test_mixed_self_product_and_bilinear` | ✓ |

## Error cases

| Scenario | Where |
|----------|-------|
| `POWER(x, 3)` (exponent ≠ 2) rejected | `test_quadratic.py` |
| Variable exponent rejected | `test_quadratic.py` |
| HiGHS rejects non-convex QP | `test_quadratic.py` (`_expect_gurobi` pattern) |
| HiGHS rejects MIQP | `test_quadratic.py` |
| HiGHS rejects quadratic constraints | `test_quadratic_constraints.py` |
| Infeasible quadratic constraint | `test_quadratic_constraints.py` |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| QP objective | WHEN | ✓ |
| QP constraint | WHEN | ✓ |
| QP constraint | PER | ✓ |
| QP objective | multiple variables | ✓ |
| QP constraint | multiple variables | ✓ |
| QP | BOOLEAN (MIQP) | ✓ (gurobi-gated) |
| QP | INTEGER (MIQP) | ✓ (gurobi-gated) |
| QP | REAL | ✓ |
| QP | entity-scoped | ✓ |
| QP constraint | bilinear (mixed) | ✓ |
| QP objective | PER constraint (flat MINIMIZE + SUM(x)>=K PER grp) | ✓ (`test_per_interactions.py::test_qp_objective_per_constraint`) |
