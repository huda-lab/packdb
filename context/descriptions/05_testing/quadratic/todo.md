# Quadratic Programming Test Coverage — Todo

## Closed

- **QP objective + PER constraint** — `test_per_interactions.py::test_qp_objective_per_constraint` (2026-04-15): flat `MINIMIZE SUM(POWER(x-t,2))` with `SUM(x)>=5 PER grp`. Targets=0.5 so PER floor binds (x→2.5, obj=16).
- **Mixed linear + quadratic objective** — `test_quadratic.py::test_qp_mixed_linear_quadratic`, `::test_qp_mixed_separate_sums`, `::test_qp_mixed_negated_quadratic` (2026-04-17). Both SUM-shapes (nested `SUM(POWER(...) + c*x)` and sibling `SUM(POWER(...)) + SUM(c*x)`) and the negated form (`MAXIMIZE SUM(-POWER(...) + c*x)`) oracle-compared against Gurobi `set_quadratic_objective`. Fixed the extraction/evaluation bug where mixed shapes silently dropped the Q matrix or coerced POWER to `1*x`. Multiple quadratic groups in one objective (`SUM(POWER(x,2)) + SUM(POWER(y,2))`) are explicitly rejected — see `test_qp_multiple_quadratic_groups_rejected`.
- **Nested-PER QP objective** — `test_quadratic.py::test_qp_nested_sum_sum_per_binding`, `::test_qp_nested_sum_sum_per_unconstrained`, `::test_qp_nested_sum_avg_per_binding`, `::test_qp_nested_sum_sum_per_constant_free_regression` (2026-04-17). `MINIMIZE SUM(SUM(POWER(x-t,2))) PER grp` and `MINIMIZE SUM(AVG(POWER(x-t,2))) PER grp` bind, optimize, and oracle-compare. Fix: `SumInnerIsQuadratic` in `src/packdb/symbolic/decide_symbolic.cpp` now recognizes a nested aggregate wrapper around quadratic (SUM/AVG/MIN/MAX of POWER), so `NormalizeDecideObjective` preserves the raw AST and the post-bind optimizer's nested-strip at `decide_optimizer.cpp:634` flattens OUTER(INNER(expr))→INNER(expr). Pre-fix, the binder rejected these with a `__SUM__` placeholder leak from constant-folded POWER expansion. SUM(MIN/MAX(POWER)) PER grp now also binds but downstream correctness at the physical layer is not verified — see "MIN/MAX of quadratic inner" below.

## Missing coverage

### MEDIUM: `SUM(MIN(POWER(expr, 2))) PER grp` and `SUM(MAX(POWER(expr, 2))) PER grp` — oracle-verified correctness

Post-fix these shapes now parse and bind (the binder blocker was resolved 2026-04-17). Smoke tests return plausible x values, but no oracle test confirms the physical layer correctly builds quadratic per-row auxiliary constraints `z_g ≶ POWER(x_i - t_i, 2)` together with the hard-inner indicator pattern `y_i=1 ⇒ z_g bound POWER_i`. Risk: silent wrong results if the physical layer emits linear auxiliaries when the inner expression is quadratic.

Test plan once confirmed: build the oracle using a custom variant of `emit_hard_inner_min`/`emit_hard_inner_max` from `tests/_oracle_helpers.py` that accepts quadratic `row_coeffs` (passed as dicts of `(linear, quadratic)`) and adds them via `oracle_solver.add_quadratic_constraint` instead of `add_constraint`. Data must be shaped so per-row x values are coupled (e.g., a binding aggregate constraint or shared resources), otherwise MIN/MAX optimum coincides with the per-row independent optimum and the test fails to distinguish from SUM.

Closed in `test_quadratic_constraints.py` (2026-04-17): Quadratic constraint with WHEN + PER combined (`SUM(POWER(expr, 2)) op K WHEN cond PER col`) — `test_when_per_quadratic_constraint`. Verifies the WHEN mask applies before PER grouping (per-group Q matrix uses only active rows; inactive rows go free to upper bound).

### LOW: Non-convex QP explicit error message test for HiGHS

The `_expect_gurobi` decorator catches any `PackDBCliError` containing "quadratic" or "Gurobi" and passes the test. There is no dedicated HiGHS-only test that verifies the *specific* error message content for non-convex QP, ensuring the user gets clear guidance.

```python
# Pseudo: direct HiGHS-only error check
@pytest.mark.skip_if_gurobi
def test_highs_rejects_nonconvex_qp_with_clear_message():
    with pytest.raises(PackDBCliError, match=r"non-convex.*Gurobi"):
        packdb.execute("MAXIMIZE SUM(POWER(x, 2)) ...")
```
