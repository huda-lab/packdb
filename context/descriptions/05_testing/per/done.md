# PER Clause Test Coverage — Done

Tests live in:
- `test/decide/tests/test_per_clause.py` — basic PER on constraints
- `test/decide/tests/test_per_multi_column.py` — multi-column PER
- `test/decide/tests/test_per_objective.py` — PER on objectives (nested aggregates)
- `test/decide/tests/test_per_interactions.py` — PER composed with auxiliary-variable features (hard MIN/MAX, ABS, multi-variable)
- `test/decide/tests/test_per_strict.py` — PER STRICT variant

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
| WHEN + PER + multi-variable — WHEN mask per group | `test_per_interactions.py::test_when_per_multi_variable` | ✓ |
| COUNT(x INTEGER) + PER — Big-M indicators per group | `test_per_interactions.py::test_count_integer_per` | ✓ |
| QP objective + PER constraint | `test_per_interactions.py::test_qp_objective_per_constraint` | ✓ |
| PER equality constraint (`SUM(x) = K PER col`, two-sided bounds per group) | `test_abs_linearization.py` | ✓ |
| Feasibility (no objective) + PER | `test_edge_cases.py::test_feasibility_per` | ✓ |
| Single-row PER groups (`|group| = 1` degenerate) | `test_per_interactions.py::test_per_single_row_groups` | ✓ |
| Zero-coefficient PER group (one group's aggregate is vacuous) | `test_per_interactions.py::test_per_zero_coefficient_group` | ✓ |
| NULL PER-key + WHEN mask (NULL bucket with WHEN→PER empty-skip) | `test_per_interactions.py::test_per_null_group_with_when` | ✓ |
| Uncorrelated scalar subquery as PER constraint RHS | `test_cons_subquery.py::test_per_constraint_with_subquery_rhs` | ✓ |

### PER on objectives (nested aggregate syntax)

All 16+ combinations of outer/inner ∈ {SUM, MIN, MAX, AVG} tested in `test_per_objective.py`:

| Scenario | Test |
|----------|------|
| `SUM(SUM(...)) PER col` (no-op equivalence) | `test_sum_sum_per_noop` |
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
| `PER` on per-row constraint (`x <= 5 PER col`) rejected | `test_error_binder.py::TestBinderErrors::test_per_on_perrow_constraint_rejection` |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| PER | WHEN (expression-level) | ✓ |
| PER | WHEN (aggregate-local) | ✓ |
| PER | multi-column | ✓ |
| PER | INTEGER variables | ✓ |
| PER | NULL group keys | ✓ |
| PER | `<>` | ✓ |
| PER | MIN/MAX (easy, stripped) | ✓ |
| PER | MIN/MAX (objective, nested) | ✓ |
| PER | AVG | ✓ |
| PER | COUNT (BOOLEAN) | ✓ |
| PER | COUNT (INTEGER) | ✓ |
| PER | entity-scoped | ✓ |
| PER | quadratic constraint | ✓ |
| PER | quadratic constraint + WHEN | ✓ |
| PER | QP objective | ✓ |
| PER | WHEN + entity-scoped (triple) | ✓ |
| PER | WHEN + MIN/MAX (triple) | ✓ |
| PER | WHEN + AVG (triple) | ✓ |
| PER | hard MIN/MAX constraints (Big-M per group) | ✓ |
| PER | ABS in aggregate constraint | ✓ |
| PER | multi-variable (BOOLEAN + INTEGER) | ✓ |
| PER | WHEN + multi-variable | ✓ |
| PER | equality constraint | ✓ |
| PER | feasibility (no objective) | ✓ |
| PER STRICT | WHEN vacuously-true upper bound | ✓ (`test_per_strict.py`) |
| PER STRICT | WHEN infeasible lower bound | ✓ |
| PER STRICT | hard MIN/MAX (existential → infeasible) | ✓ |
| PER STRICT | easy MIN/MAX (no-op) | ✓ |
| PER STRICT | NE (`<>`) | ✓ |
| PER STRICT | entity-scoped | ✓ (`test_entity_scope.py::test_entity_scoped_per_strict`) |
