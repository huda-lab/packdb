# WHEN Clause Test Coverage — Done

Tests live in:
- `test/decide/tests/test_when_constraint.py` — expression-level WHEN on constraints
- `test/decide/tests/test_when_perrow.py` — WHEN on per-row constraints
- `test/decide/tests/test_when_objective.py` — WHEN on objectives
- `test/decide/tests/test_when_compound.py` — compound AND/OR conditions
- `test/decide/tests/test_aggregate_local_when.py` — aggregate-local WHEN variant

## Scenarios covered

### WHEN on aggregate constraints

| Scenario | Where | Oracle |
|----------|-------|--------|
| String equality condition | `test_when_constraint.py::test_when_aggregate_string_equality` | ✓ |
| Numeric comparison condition | `test_when_constraint.py::test_when_aggregate_numeric_comparison` | ✓ |
| Constant coefficient in WHEN-gated SUM | `test_when_constraint.py::test_when_aggregate_constant_coeff` | ✓ |
| `<>` with WHEN (expression-level) | `test_when_constraint.py::test_when_not_equal` | ✓ |
| All rows match (trivially applies) | `test_when_constraint.py::test_when_all_rows_match` | ✓ |
| No rows match (trivially satisfied) | `test_when_constraint.py::test_when_no_rows_match` | ✓ |
| NULL in condition column | `test_when_constraint.py::test_when_null_condition_column` | ✓ |
| Explicit `IS NOT NULL` predicate in WHEN (requires parens around predicate) | `test_when_constraint.py::test_when_is_not_null_predicate` | ✓ |
| Mixed conditional + unconditional constraints | `test_when_constraint.py::test_when_mixed_conditional_and_unconditional` | ✓ |
| Multiple categories (different WHEN per constraint) | `test_when_constraint.py::test_when_multiple_categories` | ✓ |
| Constraint ordering invariance | `test_when_constraint.py::test_when_constraint_ordering_invariance` | ✓ |

### WHEN on per-row constraints

| Scenario | Where | Oracle |
|----------|-------|--------|
| Force to zero when inactive | `test_when_perrow.py` | ✓ |
| Force selection under condition | `test_when_perrow.py` | ✓ |
| Numeric row filter | `test_when_perrow.py` | ✓ |
| All rows match | `test_when_perrow.py` | ✓ |
| No rows match | `test_when_perrow.py` | ✓ |
| IS REAL variable (continuous skip-constraint path, no implicit [0,1] cap) | `test_when_perrow.py::test_when_perrow_real` | ✓ |

### WHEN on objectives

| Scenario | Where | Oracle |
|----------|-------|--------|
| MAXIMIZE + WHEN | `test_when_objective.py` | ✓ |
| MINIMIZE + WHEN | `test_when_objective.py` | ✓ |
| Unconditional constraint + WHEN objective | `test_when_objective.py` | ✓ |
| Same WHEN on constraint and objective | `test_when_objective.py` | ✓ |
| Different WHEN on constraint vs objective | `test_when_objective.py` | ✓ |
| WHEN on objective matching zero rows | `test_when_objective.py::test_when_objective_no_match` | ✓ |

### Compound conditions

| Scenario | Where | Oracle |
|----------|-------|--------|
| WHEN with AND (parenthesized) | `test_when_compound.py` | ✓ |
| WHEN with OR (parenthesized) | `test_when_compound.py` | ✓ |
| Nested compound conditions | `test_when_compound.py` | ✓ |

### Aggregate-local WHEN

| Scenario | Where | Oracle |
|----------|-------|--------|
| Independent masks on additive aggregate terms | `test_aggregate_local_when.py` | ✓ |
| Aggregate-local WHEN + AVG | `test_aggregate_local_when.py::test_aggregate_local_when_with_avg_constraint` | ✓ |
| Aggregate-local WHEN + PER | `test_aggregate_local_when.py::test_aggregate_local_when_with_per_constraint` | ✓ |
| Aggregate-local WHEN + PER + AVG | `test_aggregate_local_when.py::test_aggregate_local_when_with_avg_and_per` | ✓ |
| Aggregate-local WHEN + MAX (easy) | `test_aggregate_local_when.py::test_aggregate_local_when_with_max` | ✓ |
| Aggregate-local WHEN + MAX (hard direction `>=`, Big-M indicators restricted to WHEN-matching rows) | `test_aggregate_local_when.py::test_aggregate_local_when_with_hard_max` | ✓ |
| Aggregate-local WHEN + bilinear (constraint) | `test_aggregate_local_when.py::test_bilinear_aggregate_local_when_constraint` | ✓ |
| Aggregate-local WHEN + bilinear (objective) | `test_aggregate_local_when.py::test_bilinear_aggregate_local_when_objective` | ✓ |
| Aggregate-local WHEN + entity-scoped | `test_aggregate_local_when.py::test_entity_scoped_aggregate_local_when` | ✓ |
| Aggregate-local WHEN + BETWEEN | `test_aggregate_local_when.py::test_between_aggregate_local_when_constraint` | ✓ |
| Expression-level WHEN + PER still works | `test_aggregate_local_when.py::test_expression_level_when_per_still_works` | ✓ |
| Aggregate-local WHEN + `<>` (NE) constraint | `test_aggregate_local_when.py::test_ne_aggregate_local_when_constraint` | ✓ |
| Aggregate-local WHEN + `<>` + PER | `test_aggregate_local_when.py::test_ne_with_per_constraint` | ✓ |
| Aggregate-local WHEN objective re-association | `test_aggregate_local_when.py::test_aggregate_local_when_objective_reassociation` | ✓ |
| Aggregate-local WHEN single aggregate | `test_aggregate_local_when.py::test_aggregate_local_when_single_aggregate` | ✓ |
| Aggregate-local WHEN with three terms | `test_aggregate_local_when.py::test_aggregate_local_when_three_terms` | ✓ |
| Aggregate-local WHEN overlapping filters | `test_aggregate_local_when.py::test_aggregate_local_when_overlapping_filters` | ✓ |
| Aggregate-local WHEN all rows filtered out | `test_aggregate_local_when.py::test_aggregate_local_when_all_filtered_out` | ✓ |
| Aggregate-local WHEN objective mixing filtered + unfiltered terms | `test_aggregate_local_when.py::test_aggregate_local_when_objective_mixed_filtered_unfiltered` | ✓ |
| Aggregate-local WHEN constraint mixing filtered + unfiltered terms | `test_aggregate_local_when.py::test_aggregate_local_when_mixed_filtered_unfiltered_constraint` | ✓ |

### Error cases

| Scenario | Where |
|----------|-------|
| Mixing expression-level and aggregate-local WHEN (rejected) | `test_aggregate_local_when.py` |
| DECIDE variable in WHEN condition (rejected) | `test_error_binder.py` |
| Compound WHEN with DECIDE variable (rejected) | `test_error_binder.py` |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| WHEN | aggregate constraint | ✓ |
| WHEN | per-row constraint | ✓ (BOOLEAN, INTEGER, REAL) |
| WHEN | objective | ✓ |
| WHEN | compound AND/OR | ✓ |
| WHEN | PER | ✓ |
| WHEN | MIN/MAX (easy) | ✓ |
| WHEN | MIN/MAX (hard, aggregate-local) | ✓ |
| WHEN | AVG | ✓ |
| WHEN | ABS (objective) | ✓ |
| WHEN | QP | ✓ |
| WHEN | quadratic constraint | ✓ |
| WHEN | `<>` (expression-level) | ✓ |
| WHEN | `<>` (aggregate-local) | ✓ |
| WHEN | entity-scoped | ✓ |
| WHEN | bilinear | ✓ |
| WHEN | NULL in condition column | ✓ |
