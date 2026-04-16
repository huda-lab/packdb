# Quadratic Programming Test Coverage — Todo

## Closed

- **QP objective + PER constraint** — `test_per_interactions.py::test_qp_objective_per_constraint` (2026-04-15): flat `MINIMIZE SUM(POWER(x-t,2))` with `SUM(x)>=5 PER grp`. Targets=0.5 so PER floor binds (x→2.5, obj=16). The nested PER objective form (`SUM(SUM(POWER(x-t,2))) PER col`) remains untested (see Batch 3).

## Missing coverage

### HIGH: QP objective with nested PER (SUM of per-group QP)

The flat form is now tested. The nested form `MINIMIZE SUM(SUM(POWER(x - t, 2))) PER col` (outer SUM of inner per-group QP) creates per-group Q matrices with auxiliaries. That code path has zero coverage.

```sql
MINIMIZE SUM(SUM(POWER(x - target, 2))) PER grp
```

### MEDIUM: Mixed linear + quadratic objective

`SUM(POWER(x - t, 2) + c * x)` — the linear part contributes to `c^T x`, the quadratic part to Q. If they aren't composed properly, one overwrites the other or gets dropped.

```sql
-- Mixed linear + quadratic
SELECT id, ROUND(x, 4) AS x FROM data
DECIDE x IS REAL
SUCH THAT x >= 0 AND x <= 100
MINIMIZE SUM(POWER(x - target, 2) + penalty * x)
```

### MEDIUM: Quadratic constraint with WHEN + PER combined

WHEN is tested on one quadratic constraint test; PER is tested on another; the combination `SUM(POWER(expr, 2)) op K WHEN cond PER col` has no test.

**Risk**: The WHEN mask must apply before PER grouping for quadratic constraints just as it does for linear ones. The Q matrix must be built per-group using only WHEN-matching rows.

```sql
-- Quadratic constraint with WHEN + PER
SELECT id, grp, active, ROUND(x, 4) AS x FROM data
DECIDE x IS REAL
SUCH THAT x >= 0 AND x <= 100
    AND SUM(POWER(x - target, 2)) <= 50 WHEN active PER grp
MAXIMIZE SUM(x)
```

### LOW: Non-convex QP explicit error message test for HiGHS

The `_expect_gurobi` decorator catches any `PackDBCliError` containing "quadratic" or "Gurobi" and passes the test. There is no dedicated HiGHS-only test that verifies the *specific* error message content for non-convex QP, ensuring the user gets clear guidance.

```python
# Pseudo: direct HiGHS-only error check
@pytest.mark.skip_if_gurobi
def test_highs_rejects_nonconvex_qp_with_clear_message():
    with pytest.raises(PackDBCliError, match=r"non-convex.*Gurobi"):
        packdb.execute("MAXIMIZE SUM(POWER(x, 2)) ...")
```
