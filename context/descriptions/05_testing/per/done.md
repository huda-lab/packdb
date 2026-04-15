# PER Clause Test Coverage — Done

Tests live in:
- `test/decide/tests/test_per_clause.py` — basic PER on constraints
- `test/decide/tests/test_per_multi_column.py` — multi-column PER
- `test/decide/tests/test_per_objective.py` — PER on objectives (nested aggregates)
- `test/decide/tests/test_per_interactions.py` — PER composed with auxiliary-variable features (hard MIN/MAX, ABS, multi-variable)

## Scenarios covered

### PER on constraints

| Scenario | Where | Oracle |
|----------|-------|--------|
| Single-column PER with aggregate constraint | `test_per_clause.py` | ✓ |
| Multi-column PER (2 columns) | `test_per_multi_column.py` | ✓ |
| Multi-column PER (3 columns) | `test_per_multi_column.py` | ✓ |
| PER with INTEGER variables | `test_per_clause.py` | ✓ |
| PER + WHEN (combined) | `test_per_clause.py::test_per_combined_with_when`, `test_per_multi_column.py` | ✓ |
| PER + `<>` | `test_per_clause.py::test_per_not_equal` | ✓ |
| NULL group keys (excluded) | `test_per_clause.py::test_per_null_group_key` | ✓ |
| Two PER constraints on different columns | `test_per_clause.py::test_per_different_grouping_columns` | ✓ |
| WHEN filters out entire PER group | `test_per_multi_column.py::test_multi_column_per_when_eliminates_all_in_group` | ✓ |
| PER + hard MAX(>=K) constraint (Big-M per group) | `test_per_interactions.py::test_per_max_geq_constraint` | ✓ |
| PER + hard MIN(<=K) constraint (Big-M per group) | `test_per_interactions.py::test_per_min_leq_constraint` | ✓ |
| PER + equality MAX(=K) constraint (easy + hard combined per group) | `test_per_interactions.py::test_per_max_eq_constraint` | ✓ |
| PER + ABS in aggregate constraint (ABS aux per group) | `test_per_interactions.py::test_per_abs_aggregate` | ✓ |
| Multi-variable (BOOLEAN + INTEGER) + PER | `test_per_interactions.py::test_per_multi_variable` | ✓ |

### PER on objectives (nested aggregate syntax)

All 16+ combinations of outer/inner ∈ {SUM, MIN, MAX, AVG} tested in `test_per_objective.py`:

| Scenario | Test |
|----------|------|
| `SUM(SUM(...)) PER col` (no-op equivalence) | `test_sum_sum_per` |
| `MINIMIZE SUM(MAX(...)) PER col` | `test_minimize_sum_max_per` |
| `MAXIMIZE SUM(MIN(...)) PER col` | `test_maximize_sum_min_per` |
| `MAXIMIZE SUM(MAX(...)) PER col` (hard outer + easy inner) | `test_maximize_sum_max_per` |
| `MINIMIZE SUM(MIN(...)) PER col` (hard outer + easy inner) | `test_minimize_sum_min_per` |
| `MINIMIZE MAX(SUM(...)) PER col` | `test_minimize_max_sum_per` |
| `MAXIMIZE MIN(SUM(...)) PER col` | `test_maximize_min_sum_per` |
| `SUM(AVG(...)) PER col` with unequal groups | `test_sum_avg_per_unequal_groups` |
| `MINIMIZE MAX(AVG(...)) PER col` | `test_minimize_max_avg_per` |
| `MAXIMIZE MIN(AVG(...)) PER col` | `test_maximize_min_avg_per` |
| Nested + WHEN: `SUM(MAX(...)) WHEN ... PER col` | `test_sum_max_when_per` |
| Single-group degenerate case | `test_single_group` |

### Error cases

| Scenario | Where |
|----------|-------|
| Flat `MIN/MAX + PER` (ambiguous) rejected | `test_per_objective.py` |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| PER | WHEN (expression-level) | ✓ |
| PER | WHEN (aggregate-local) | ✓ (`test_aggregate_local_when.py`) |
| PER | multi-column | ✓ |
| PER | INTEGER variables | ✓ |
| PER | NULL group keys | ✓ |
| PER | `<>` | ✓ |
| PER | MIN/MAX (easy, stripped) | ✓ |
| PER | MIN/MAX (objective, nested) | ✓ |
| PER | AVG | ✓ |
| PER | COUNT (BOOLEAN) | ✓ |
| PER | entity-scoped | ✓ (`test_entity_scope.py`) |
| PER | quadratic constraint | ✓ (`test_quadratic_constraints.py`) |
| PER | WHEN + entity-scoped (triple) | ✓ |
| PER | WHEN + MIN/MAX (triple) | ✓ |
| PER | WHEN + AVG (triple) | ✓ |
| PER | hard MIN/MAX constraints (Big-M per group) | ✓ (`test_per_interactions.py`) |
| PER | ABS in aggregate constraint | ✓ (`test_per_interactions.py`) |
| PER | multi-variable (BOOLEAN + INTEGER) | ✓ (`test_per_interactions.py`) |
