# Test Coverage Documentation

Tracks the state of the DECIDE test suite — what is covered, what gaps remain,
and which tests verify correctness (oracle comparison) vs. feasibility only.

For **how** the testing framework works (oracle solver, fixtures, comparison
logic), see [../02_operations/oracle.md](../02_operations/oracle.md). This
folder answers **what** is being tested and what isn't.

## Structure

Each subdirectory corresponds to a feature area and contains:
- `done.md` — covered scenarios, with oracle vs. constraint-only status
- `todo.md` — remaining gaps and upgrade candidates

## Areas

| Directory | Feature | Key test files |
|-----------|---------|----------------|
| `variables/` | IS BOOLEAN / IS INTEGER / IS REAL, multi-variable | `test_var_*.py` |
| `entity_scope/` | Table-scoped decision variables (`DECIDE Table.var`) | `test_entity_scope.py` |
| `constraints/` | Comparison operators (`=`, `<`, `<=`, `>`, `>=`, `<>`, BETWEEN, IN), per-row vs aggregate | `test_cons_*.py` |
| `when/` | WHEN clause (expression-level and aggregate-local) | `test_when_*.py`, `test_aggregate_local_when.py` |
| `per/` | PER clause (grouped constraints and nested-aggregate objectives) | `test_per_*.py` |
| `min_max/` | MIN/MAX linearization (easy/hard cases, nested with PER) | `test_min_max.py`, `test_per_objective.py` |
| `avg/` | AVG aggregate (execution-time coefficient scaling) | `test_avg.py` |
| `count/` | COUNT aggregate (BOOLEAN → SUM, INTEGER → Big-M) | `test_count_*.py` |
| `abs/` | ABS linearization | `test_abs_linearization.py` |
| `bilinear/` | Bilinear terms `x * y` (McCormick for Bool × anything, Q-matrix otherwise) | `test_bilinear.py` |
| `quadratic/` | QP objectives, QCQP constraints | `test_quadratic.py`, `test_quadratic_constraints.py` |
| `subquery/` | Scalar and correlated subqueries as constraint RHS | `test_cons_subquery.py`, `test_cons_correlated_subquery.py` |
| `error_handling/` | Parser errors, binder errors, infeasibility, solver-specific errors | `test_error_*.py` |
| `edge_cases/` | Boundary conditions, data shapes, scale, JOIN sources | `test_edge_cases.py`, `test_large_scale.py`, `test_sql_*.py` |

## How to use

When implementing a new feature or fixing a bug in a tested area:
1. Check the area's `done.md` to understand existing coverage.
2. Check the area's `todo.md` for known gaps before writing new tests.
3. After adding/changing tests, update `done.md` to reflect new state.
4. Move items from `todo.md` to `done.md` as they are completed.

When planning test work from scratch, start with the HIGH-risk gaps across all
`todo.md` files — these are where silent correctness bugs are most likely.

## Coverage quality levels

- **oracle** (✓) — PackDB objective is compared against an independent reference solver. Catches silent optimality bugs (wrong coefficients, wrong constraint direction, rewrite errors).
- **constraint only** — Test checks that constraints are satisfied and the result is non-empty. Does not verify the solution is optimal. Can miss bugs where the solver produces a valid but suboptimal solution due to incorrect formulation.
- **xfail** — Known defect or feature not yet implemented. Test is kept to auto-detect when fixed.
- **error test** — Verifies that an invalid query is rejected with the right error message.

## Risk priorities for gaps

HIGH-risk gaps involve:
- Optimizer rewrites interacting with another feature (PER+bilinear, Big-M+PER, Q matrix+PER)
- Linearization auxiliary variable indexing across features
- Boundary conditions that could crash or hang

MEDIUM-risk gaps involve:
- Documented features used standalone without tests
- Untested but plausible feature pairs
- Error paths without explicit coverage

LOW-risk gaps involve:
- Edge cases unlikely in practice
- Stress tests of structural limits
- Error-message quality / regex tightening

## Audit history

- **2026-04-15**: Broad audit across the full feature surface (see individual `todo.md` files for findings). Future audits can be regenerated via the `/test-review` skill.
