# Constraint Operator Test Coverage — Done

Covers the constraint operators `=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`, and `IN`, plus structural forms (per-row, aggregate, multi-constraint, mixed). Tests live in:
- `test/decide/tests/test_cons_comparison.py` — all 6 comparison operators
- `test/decide/tests/test_cons_between.py` — BETWEEN
- `test/decide/tests/test_cons_in.py` — IN on decision variables
- `test/decide/tests/test_cons_perrow.py` — per-row constraints
- `test/decide/tests/test_cons_aggregate.py` — aggregate (SUM) constraints
- `test/decide/tests/test_cons_mixed.py` — per-row + aggregate combined
- `test/decide/tests/test_cons_multi.py` — multiple aggregate constraints

## Scenarios covered

### Comparison operators

| Operator | On per-row | On aggregate | Where |
|----------|-----------|-------------|-------|
| `=` | ✓ | ✓ | `test_cons_comparison.py` |
| `<` | ✓ | ✓ | `test_cons_comparison.py` |
| `<=` | ✓ | ✓ | many files |
| `>` | ✓ | ✓ | `test_cons_comparison.py` |
| `>=` | ✓ | ✓ | many files |
| `<>` | ✓ | ✓ | `test_cons_comparison.py` (expression-level) |
| `<>` + WHEN (expression-level) | — | ✓ | `test_cons_comparison.py::test_sum_not_equal_with_when` |
| `<>` + WHEN (aggregate-local) | — | ✓ | `test_aggregate_local_when.py::test_ne_aggregate_local_when_constraint` |
| `<>` with WHEN binding | — | ✓ | `test_cons_comparison.py::test_sum_not_equal_with_when_binding` |
| `<>` without WHEN binding | — | ✓ | `test_cons_comparison.py::test_sum_not_equal_no_when_binding` |
| `<>` with mixed-sign coefficient column (both-sided disjunction) | — | ✓ | `test_cons_comparison.py::test_sum_not_equal_mixed_sign_coeffs` |
| `<` / `>` on REAL LHS → rejected | ✓ | ✓ | `test_cons_comparison.py::test_real_sum_strict_lt_rejected`, `test_real_sum_strict_gt_rejected`, `test_real_perrow_strict_rejected` |
| `<` / `>` with non-integer coefficient → rejected | — | ✓ | `test_cons_comparison.py::test_integer_fractional_coeff_strict_rejected` |
| `<` / `>` on mixed BOOL + REAL LHS → rejected | — | ✓ | `test_cons_comparison.py::test_mixed_bool_real_strict_rejected` |
| `<` / `>` on `SUM(POWER(real, 2))` (quadratic path) → rejected | — | ✓ | `test_cons_comparison.py::test_strict_lt_rejection_quadratic_real` |
| `<` / `>` on `SUM(bool × real)` (bilinear path) → rejected | — | ✓ | `test_cons_comparison.py::test_strict_lt_rejection_bilinear_bool_real` |
| `<>` on REAL LHS → rejected | — | ✓ | `test_cons_comparison.py::test_real_sum_not_equal_rejected` |
| `<` / `>` on BOOLEAN × INTEGER bilinear LHS → solves (oracle) | — | ✓ | `test_cons_comparison.py::test_bilinear_bool_int_strict_oracle` |
| `<` / `>` on INTEGER × INTEGER bilinear LHS → solves (oracle, Gurobi) | — | ✓ | `test_cons_comparison.py::test_bilinear_int_int_strict_oracle` |
| `<` / `>` on pure INTEGER SUM → solves (oracle) | ✓ | ✓ | `test_cons_comparison.py::test_integer_sum_strict_lt_oracle`, `test_integer_perrow_strict_oracle` |
| `<` / `>` on `SUM(ABS(integer_expr))` → solves (oracle) | — | ✓ | `test_cons_comparison.py::test_abs_integer_strict_oracle` |
| `<=` / `>=` on REAL LHS still work (regression) | — | ✓ | `test_cons_comparison.py::test_real_sum_le_still_works` |

### BETWEEN

| Scenario | Where | Oracle |
|----------|-------|--------|
| Per-row BETWEEN (`x BETWEEN a AND b`) | `test_cons_between.py` | ✓ |
| Per-row BETWEEN with fractional bounds on REAL var | `test_cons_between.py::test_real_between_oracle` | ✓ |
| Column-derived BETWEEN (`x BETWEEN 0 AND col`) | `test_cons_between.py::test_q10_logic_dependency` | ✓ |
| Multi-constraint + BETWEEN + aggregate | `test_cons_between.py::test_q10_logic_dependency` | ✓ |
| Aggregate BETWEEN (standalone desugar — two constraints from one BETWEEN) | `test_cons_between.py::test_aggregate_between_standalone` | ✓ |
| Aggregate BETWEEN (inside aggregate-local WHEN) | `test_aggregate_local_when.py::test_between_aggregate_local_when_constraint` | ✓ |
| BETWEEN + entity-scoped | `test_entity_scope.py::test_entity_scoped_between_constraint` | constraint only |
| BETWEEN RHS non-scalar (rejected) | `test_error_binder.py::test_between_non_scalar` | error test |
| DECIDE var in BETWEEN bounds (rejected) | `test_error_binder.py::test_decide_between_decide_variable` | error test |

### IN

| Scenario | Where | Oracle |
|----------|-------|--------|
| `x IN (values)` on decision variable | `test_cons_in.py` | ✓ |
| `x IN (non-integer values)` on REAL variable | `test_cons_in.py::test_real_in_oracle` | ✓ |
| `x IN (0, 1)` on BOOLEAN (no-op optimization) | `test_cons_in.py` | ✓ |
| `x IN (single_value)` → rewritten to `x = v` | `test_cons_in.py` | ✓ |
| IN + WHEN composition | `test_cons_in.py` | ✓ |
| `SUM(x) IN (...)` rejected | `test_error_binder.py::test_sum_with_in_not_allowed` | error test |
| DECIDE var in IN RHS rejected | `test_error_binder.py::test_in_rhs_with_decide_variable` | error test |

### Structural forms

| Scenario | Where | Oracle |
|----------|-------|--------|
| Per-row bounds (`x <= 5`) | `test_cons_perrow.py` | ✓ |
| Aggregate constraints (`SUM(x * col) <= K`) | `test_cons_aggregate.py` | ✓ |
| Per-row + aggregate combined | `test_cons_mixed.py::test_q02_integer_procurement` | ✓ |
| Multiple aggregate constraints | `test_cons_multi.py::test_q06_multi_constraint` | ✓ |
| Subquery RHS | `test_cons_subquery.py::test_q04_subquery_rhs` | ✓ |
| Correlated subquery RHS | `test_cons_correlated_subquery.py` | ✓ (6 tests) |

### Edge cases

| Scenario | Where | Oracle |
|----------|-------|--------|
| RHS = 0 forces all zero | `test_edge_cases.py::test_rhs_zero_forces_all_zero` | ✓ |
| Negative objective coefficients | `test_edge_cases.py::test_negative_objective_coefficients` | ✓ |
| Negative coefficients in aggregate **constraint** (signed column) | `test_cons_comparison.py::test_sum_negative_coeffs_aggregate` | ✓ |
| Negative constant literal multiplier in aggregate constraint (`SUM(x * (-k)) <= -K`) | `test_cons_comparison.py::test_sum_negative_constant_multiplier` | ✓ |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| Comparison operators | aggregate SUM | ✓ (all 6) |
| Comparison operators | per-row | ✓ (all 6) |
| `<>` | PER | ✓ |
| `<>` | entity-scoped | ✓ |
| `<>` | REAL LHS (rejected) | ✓ |
| BETWEEN | entity-scoped | ✓ |
| BETWEEN | aggregate-local WHEN | ✓ |
| BETWEEN | PER + REAL var (fractional bounds) | ✓ (`test_per_clause.py::test_real_between_per_oracle`) |
| IN | WHEN | ✓ |
| IN | BOOLEAN domain restriction | ✓ |
| Negative coefficients | objective | ✓ |
| Negative coefficients | aggregate constraint (signed column) | ✓ |
| Negative coefficients | aggregate constraint (constant literal) | ✓ |
| Multiple constraints | different operators | ✓ |
