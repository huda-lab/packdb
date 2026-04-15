# Error Handling Test Coverage — Done

Tests live in:
- `test/decide/tests/test_error_parser.py` — parser-level syntax errors (4 tests)
- `test/decide/tests/test_error_binder.py` — binder-level semantic errors (~19 tests)
- `test/decide/tests/test_error_infeasible.py` — infeasible model detection (4 tests)

## Parser errors

| Scenario | Where |
|----------|-------|
| DECIDE without SUCH THAT | `test_error_parser.py::test_missing_such_that` |
| DECIDE IS BOOLEAN without name | `test_error_parser.py::test_missing_variable_name` |
| SUCH THAT without DECIDE | `test_error_parser.py::test_missing_decide_keyword` |
| Empty constraint list | `test_error_parser.py::test_empty_constraint` |

All expect `packdb.ParserException`.

## Binder errors

| Category | Tests |
|----------|-------|
| Name/scope conflicts | `test_variable_conflicts_with_column`, `test_duplicate_decide_variables`, `test_unknown_variable_in_constraint` |
| Unsupported syntax in SUCH THAT | `test_is_null_unsupported`, `test_sum_with_in_not_allowed`, `test_non_decide_variable_in_constraint`, `test_non_sum_function_in_constraint`, `test_count_real_rejected` |
| Non-linear / invalid aggregate | `test_no_decide_variable_in_sum`, `test_multiple_decide_variables_in_sum`, `test_nonlinear_decide_variables` |
| RHS shape | `test_between_non_scalar`, `test_decide_between_decide_variable`, `test_in_rhs_with_decide_variable`, `test_sum_rhs_non_scalar`, `test_decide_variable_rhs_with_decide`, `test_sum_equal_non_scalar` |
| Objective rejections | `test_objective_with_addition`, `test_objective_bare_column` |
| Subquery | `test_subquery_rhs_non_scalar` |
| WHEN restrictions | DECIDE var in WHEN condition (aggregate + compound), correlated subquery non-scalar |
| Aggregate-local WHEN | Mixing expression-level and aggregate-local, DECIDE var in local condition |
| Flat MIN/MAX + PER | Rejected via `test_per_objective.py` |

All expect `packdb.InvalidInputException` or `packdb.BinderException`.

## Infeasibility detection

| Scenario | Where |
|----------|-------|
| Contradictory bounds (`x >= 10 AND x <= 5`) | `test_error_infeasible.py::test_contradictory_bounds` |
| Impossible SUM (`SUM(x) >= 1000` with too few rows) | `test_error_infeasible.py::test_impossible_sum` |
| Conflicting aggregate (`SUM(x) >= 100 AND SUM(x) <= 1`) | `test_error_infeasible.py::test_conflicting_aggregate` |
| Negative SUM upper bound | `test_error_infeasible.py` |
| WHEN-forced infeasibility | `test_error_infeasible.py` |

All expect `packdb.InvalidInputException` matching `"infeasible"`.

## Solver-specific error paths

| Scenario | Where |
|----------|-------|
| HiGHS rejects non-convex QP (via `_expect_gurobi`) | `test_quadratic.py` |
| HiGHS rejects MIQP (via `_expect_gurobi`) | `test_quadratic.py` |
| HiGHS rejects quadratic constraints | `test_quadratic_constraints.py` |
| HiGHS rejects non-convex bilinear | `test_bilinear.py` |
| Bilinear without upper bound on non-Boolean | `test_bilinear.py` |
| Triple product rejected | `test_bilinear.py` |
| `POWER(x, 3)` and variable exponents rejected | `test_quadratic.py` |
