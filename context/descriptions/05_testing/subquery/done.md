# Subquery Test Coverage — Done

Tests live in:
- `test/decide/tests/test_cons_subquery.py` — uncorrelated scalar subquery RHS
- `test/decide/tests/test_cons_correlated_subquery.py` — correlated subquery RHS (6 tests)
- `test/decide/tests/test_sql_subquery.py` — SQL subquery features

## Scenarios covered

### Uncorrelated subqueries

| Scenario | Where | Oracle |
|----------|-------|--------|
| Scalar subquery as constraint RHS | `test_cons_subquery.py::test_q04_subquery_rhs` | ✓ |
| Aggregate subquery (AVG, SUM, etc.) | `test_cons_subquery.py` | ✓ |
| Scalar subquery as PER constraint RHS (shared across all groups) | `test_cons_subquery.py::test_per_constraint_with_subquery_rhs` | ✓ |

### Correlated subqueries

| Scenario | Where | Oracle |
|----------|-------|--------|
| Correlated per-row bound | `test_cons_correlated_subquery.py::test_correlated_subquery_perrow_bound` | ✓ |
| Correlated AVG subquery as per-row bound on IS REAL | `test_cons_correlated_subquery.py::test_correlated_subquery_is_real` | ✓ |
| Correlated boolean gate | `test_cons_correlated_subquery.py::test_correlated_subquery_boolean_filter` | ✓ |
| Correlated objective coefficients | `test_cons_correlated_subquery.py::test_correlated_subquery_objective` | ✓ |
| Correlated + WHEN composition | `test_cons_correlated_subquery.py::test_correlated_subquery_when_composition` | ✓ |
| NULL-producing correlated with COALESCE | `test_cons_correlated_subquery.py::test_correlated_subquery_null_coalesce` | ✓ |

### Error cases

| Scenario | Where |
|----------|-------|
| Correlated aggregate RHS non-scalar | `test_error_binder.py::test_sum_rhs_non_scalar` |
| Correlated PER RHS non-scalar | `test_error_binder.py` |
| Subquery referencing DECIDE variable (tight match) | `test_error_binder.py::test_subquery_rhs_non_scalar` |
| Scalar subquery returns multiple rows | `test_error_binder.py::test_subquery_rhs_returns_multiple_rows` |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| Uncorrelated scalar subquery | constraint RHS | ✓ |
| Uncorrelated scalar subquery | PER constraint RHS | ✓ |
| Correlated subquery | per-row bound | ✓ |
| Correlated subquery | aggregate constraint | ✓ |
| Correlated subquery | objective | ✓ |
| Correlated subquery | WHEN | ✓ |
| Correlated subquery | BOOLEAN | ✓ |
| Correlated subquery | INTEGER | ✓ |
| Correlated subquery | REAL | ✓ |
| Subquery | DECIDE variable (rejected) | ✓ (error test) |

## Caveats

Per [oracle.md](../../02_operations/oracle.md) section 9: subqueries inside
SUCH THAT don't inherit the connection's `search_path`. Tables must be fully
qualified (e.g. `tpch.customer`) when subqueries reference cross-schema data.
