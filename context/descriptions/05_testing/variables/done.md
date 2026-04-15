# Variable Type Test Coverage — Done

Covers `IS BOOLEAN`, `IS INTEGER`, `IS REAL`, and multi-variable queries. Tests live in:
- `test/decide/tests/test_var_boolean.py` — IS BOOLEAN
- `test/decide/tests/test_var_integer.py` — default/IS INTEGER
- `test/decide/tests/test_var_real.py` — IS REAL
- `test/decide/tests/test_var_multi.py` — multiple variables

Note: table-scoped variables (`DECIDE Table.var`) have their own folder at
[../entity_scope/](../entity_scope/).

## Scenarios covered

### IS BOOLEAN

| Scenario | Where | Oracle |
|----------|-------|--------|
| 0/1 knapsack (classic) | `test_var_boolean.py::test_q01_knapsack_binary` | ✓ |
| Knapsack variant with weight limit | `test_var_boolean.py::test_knapsack_lineitem` | ✓ |
| Coverage across all constraint types | many files | ✓ |
| MAXIMIZE / MINIMIZE objectives | many files | ✓ |

### IS INTEGER

| Scenario | Where | Oracle |
|----------|-------|--------|
| Default type (no annotation) | `test_var_integer.py::test_simple_test` | ✓ |
| Explicit `IS INTEGER` | `test_var_integer.py` | ✓ |
| Per-row upper bound + aggregate | `test_cons_perrow.py::test_q07_row_wise_bounds` | ✓ |
| Column-derived upper bound (`x <= ps_availqty`) | `test_cons_mixed.py::test_q02_integer_procurement` | ✓ |
| BETWEEN with INTEGER | `test_cons_between.py::test_q10_logic_dependency` | ✓ |

### IS REAL

| Scenario | Where | Oracle |
|----------|-------|--------|
| Basic LP (continuous MAXIMIZE) | `test_var_real.py` | ✓ |
| Upper bound on REAL | `test_var_real.py` | ✓ |
| Mixed BOOLEAN + REAL | `test_var_real.py::test_real_mixed` | ✓ |
| REAL + WHEN on aggregate constraint | `test_var_real.py` | ✓ |
| REAL + PER on aggregate constraint | `test_var_real.py` | ✓ |

### Multiple variables

| Scenario | Where | Oracle |
|----------|-------|--------|
| Two variables with separate constraints (BOOL + INTEGER) | `test_var_multi.py::test_two_variables_separate_constraints` | ✓ |
| Mixed BOOLEAN + REAL in same query | `test_var_real.py::test_real_mixed` | ✓ |
| Mixed BOOLEAN + REAL with ABS | `test_abs_linearization.py::test_abs_mixed_vars` | ✓ |

## Error cases

| Scenario | Where |
|----------|-------|
| Variable name conflicts with column | `test_error_binder.py::test_variable_conflicts_with_column` |
| Duplicate DECIDE variable | `test_error_binder.py::test_duplicate_decide_variables` |
| Unknown variable in constraint | `test_error_binder.py::test_unknown_variable_in_constraint` |
| Unknown type annotation | `test_error_parser.py::test_missing_such_that` (related) |
| `COUNT(x REAL)` rejected | `test_error_binder.py::test_count_real_rejected` |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| BOOLEAN | all features (broadly) | ✓ |
| INTEGER | all features (broadly) | ✓ |
| REAL | MAXIMIZE objective | ✓ |
| REAL | PER constraint | ✓ |
| REAL | WHEN constraint (aggregate) | ✓ |
| BOOLEAN + INTEGER | same query | ✓ |
| BOOLEAN + REAL | same query | ✓ |
| BOOLEAN + REAL | ABS linearization | ✓ |
| BOOLEAN + REAL | bilinear (McCormick) | ✓ |
| INTEGER + REAL | QP constraint | ✓ (`test_quadratic_constraints.py`) |
