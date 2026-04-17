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
| QP objective + PER constraint (`SUM(x)>=5 PER grp` with `MINIMIZE SUM(POWER(x-t,2))`) | `test_per_interactions.py::test_qp_objective_per_constraint` | ✓ |
| Mixed linear + quadratic: `SUM(POWER(x-t,2) + c*x)` | `test_quadratic.py::test_qp_mixed_linear_quadratic` | ✓ |
| Mixed linear + quadratic: `SUM(POWER(x-t,2)) + SUM(c*x)` | `test_quadratic.py::test_qp_mixed_separate_sums` | ✓ |
| Mixed negated quadratic + linear: `MAXIMIZE SUM(-POWER(x-t,2) + c*x)` | `test_quadratic.py::test_qp_mixed_negated_quadratic` | ✓ |
| MIQP (integer vars + convex QP, maximize concave Q) | `test_quadratic.py::test_maximize_convex_power_integer_case_b` | ✓ (gurobi-gated) |
| QP + multiple variables | `test_quadratic.py::test_qp_multiple_variables` | ✓ |
| TPC-H data QP | `test_quadratic.py` | ✓ |
| Nested `SUM(SUM(POWER(x-t,2))) PER grp` with binding per-group cap | `test_quadratic.py::test_qp_nested_sum_sum_per_binding` | ✓ |
| Nested `SUM(SUM(POWER(x-t,2))) PER grp` unconstrained | `test_quadratic.py::test_qp_nested_sum_sum_per_unconstrained` | ✓ |
| Nested `SUM(AVG(POWER(x-t,2))) PER grp` with unequal groups + per-group cap | `test_quadratic.py::test_qp_nested_sum_avg_per_binding` | ✓ |
| Nested `SUM(SUM(POWER(x,2))) PER grp` (constant-free regression) | `test_quadratic.py::test_qp_nested_sum_sum_per_constant_free_regression` | ✓ |

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
| Quadratic constraint + PER groups | `test_quadratic_constraints.py::test_per_group_quadratic_constraint` | ✓ |
| Quadratic constraint + WHEN + PER (mask before group) | `test_quadratic_constraints.py::test_when_per_quadratic_constraint` | ✓ |
| Multiple quadratic constraints per query | `test_quadratic_constraints.py::test_multiple_quadratic_constraints` | ✓ |
| QCQP: quadratic objective + quadratic constraint | `test_quadratic_constraints.py::test_qcqp_quadratic_objective_and_constraint` | ✓ |
| Mixed linear + quadratic constraint | `test_quadratic_constraints.py::test_mixed_linear_and_quadratic_constraints` | ✓ |
| Table-scoped (entity) + QP | `test_quadratic_constraints.py::test_table_scoped_variables` | ✓ |
| Infeasible quadratic constraint (negative budget) | `test_quadratic_constraints.py::test_infeasible_negative_budget` | error test |
| REAL variable + QP constraint | `test_quadratic_constraints.py` | ✓ |
| INTEGER variable + QP constraint | `test_quadratic_constraints.py` | ✓ |
| Bilinear + self-product mixed | `test_quadratic_constraints.py::test_mixed_self_product_and_bilinear` | ✓ |

## Error cases

| Scenario | Where |
|----------|-------|
| `POWER(x, 3)` (exponent ≠ 2) rejected | `test_quadratic.py` |
| Variable exponent rejected | `test_quadratic.py` |
| Multiple POWER groups in one objective (e.g. `SUM(POWER(x,2)) + SUM(POWER(y,2))`) rejected | `test_quadratic.py::test_qp_multiple_quadratic_groups_rejected` |
| Degree > 2 self-product in objective (`POWER(x,2)*POWER(x,2)` = x⁴) rejected | `test_quadratic.py::test_qp_self_product_of_power_rejected` |
| Degree > 2 product of two POWERs in objective (`POWER(x,2)*POWER(y,2)` = x²y²) rejected | `test_quadratic.py::test_qp_product_of_two_powers_rejected` |
| Degree > 2 variable × POWER in objective (`a*POWER(y,2)`, cubic) rejected | `test_quadratic.py::test_qp_variable_times_power_rejected` |
| Degree > 2 self-product in constraint (`SUM(POWER(x,2)*POWER(x,2)) <= K`) rejected | `test_quadratic_constraints.py::test_constraint_self_product_of_power_rejected` |
| Degree > 2 product of two POWERs in constraint | `test_quadratic_constraints.py::test_constraint_product_of_two_powers_rejected` |
| Degree > 2 variable × POWER in constraint | `test_quadratic_constraints.py::test_constraint_variable_times_power_rejected` |
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
| QP constraint | WHEN + PER (mask before group) | ✓ (`test_quadratic_constraints.py::test_when_per_quadratic_constraint`) |
| QP objective | multiple variables | ✓ |
| QP constraint | multiple variables | ✓ |
| QP | BOOLEAN (MIQP) | ✓ (gurobi-gated) |
| QP | INTEGER (MIQP) | ✓ (gurobi-gated) |
| QP | REAL | ✓ |
| QP | entity-scoped | ✓ |
| QP constraint | bilinear (mixed) | ✓ |
| QP objective | PER constraint (flat MINIMIZE + SUM(x)>=K PER grp) | ✓ (`test_per_interactions.py::test_qp_objective_per_constraint`, oracle-solved) |
| QP objective | linear terms in same SUM (`SUM(POWER(x-t,2) + c*x)`) | ✓ (`test_quadratic.py::test_qp_mixed_linear_quadratic`) |
| QP objective | linear terms in sibling SUM (`SUM(POWER(...)) + SUM(c*x)`) | ✓ (`test_quadratic.py::test_qp_mixed_separate_sums`) |
| Negated QP objective | linear terms (`MAXIMIZE SUM(-POWER(...) + c*x)`) | ✓ (`test_quadratic.py::test_qp_mixed_negated_quadratic`) |
| QP objective | nested PER outer-SUM (`SUM(SUM(POWER(...))) PER grp`, `SUM(AVG(POWER(...))) PER grp`) | ✓ (`test_quadratic.py::test_qp_nested_sum_sum_per_binding`, `test_qp_nested_sum_avg_per_binding`). Fix: `SumInnerIsQuadratic` recognizes nested aggregate-of-quadratic so normalization preserves the raw AST; post-bind optimizer's nested-strip flattens to flat QP+PER. |
