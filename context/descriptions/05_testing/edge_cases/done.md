# Edge Cases & Data Shapes Test Coverage — Done

Tests live in:
- `test/decide/tests/test_edge_cases.py` — boundary conditions and degenerate inputs
- `test/decide/tests/test_large_scale.py` — scale/performance tests
- `test/decide/tests/test_sql_joins.py` — JOIN sources
- `test/decide/tests/test_sql_subquery.py` — SQL subquery features
- `test/decide/tests/test_explain.py` — EXPLAIN output

## Boundary conditions

| Scenario | Where | Oracle |
|----------|-------|--------|
| Zero rows matching (empty result) | `test_edge_cases.py::test_zero_rows_empty_input` | ✓ |
| Single row input (trivial problem) | `test_edge_cases.py::test_single_row` | ✓ |
| All variables forced to same value (RHS=0) | `test_edge_cases.py::test_rhs_zero_forces_all_zero` | ✓ |
| Trivially loose constraint (all selected) | `test_edge_cases.py::test_trivial_all_selected` | ✓ |
| Negative objective coefficients | `test_edge_cases.py::test_negative_objective_coefficients` | ✓ |
| NULL coefficients (with COALESCE hint) | `test_edge_cases.py::test_null_coefficients` | error test |
| Feasibility problem (no objective) | `test_edge_cases.py::test_feasibility_no_objective` | ✓ |

## Data shapes

| Scenario | Where | Oracle |
|----------|-------|--------|
| NULL in WHEN condition columns | `test_when_constraint.py::test_when_null_condition_column` | ✓ |
| NULL in PER column | `test_per_clause.py::test_per_null_group_key` | ✓ |
| Single group in PER | `test_per_objective.py::test_single_group` | ✓ |
| WHEN matching no rows | `test_when_constraint.py::test_when_no_rows_match` | ✓ |
| WHEN matching all rows | `test_when_constraint.py::test_when_all_rows_match` | ✓ |
| WHEN eliminates all in a PER group | `test_per_multi_column.py::test_multi_column_per_when_eliminates_all_in_group` | ✓ |
| Mixed conditional + unconditional | `test_when_constraint.py::test_when_mixed_conditional_and_unconditional` | ✓ |
| Multiple WHEN conditions on different constraints | `test_when_constraint.py::test_when_multiple_categories` | ✓ |
| Multiple PER constraints, different columns | `test_per_clause.py::test_per_different_grouping_columns` | ✓ |

## JOIN sources

| Scenario | Where | Oracle |
|----------|-------|--------|
| 2-table JOIN | `test_sql_joins.py::test_q05_join_decide` | ✓ |
| 3-table JOIN | `test_sql_joins.py` | ✓ |
| JOIN + entity-scoped (many tests) | `test_entity_scope.py` | ✓ |

## Scale / performance

| Scenario | Where | Oracle |
|----------|-------|--------|
| 501-row knapsack | `test_large_scale.py::test_knapsack_large` | ✓ |
| 2204-row order selection | `test_large_scale.py::test_order_selection_large` | ✓ |
| 1500-customer problem | `test_entity_scope.py::test_entity_scoped_mixed_when_per` | constraint only (HiGHS reliability at scale) |

## Solver coverage

From `test/decide/solver/factory.py`: the oracle picks ONE solver per session
(Gurobi if `gurobipy` is installed, HiGHS otherwise). The `_expect_gurobi`
decorator in QP/bilinear tests skips or accepts-error on HiGHS.

## Infrastructure / meta-tests

| Scenario | Where |
|----------|-------|
| EXPLAIN output format | `test_explain.py` |
| SQL subquery features (non-DECIDE) | `test_sql_subquery.py` |
