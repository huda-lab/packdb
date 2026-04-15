# Error Handling Test Coverage — Todo

## Missing coverage

### HIGH: Unbounded problem detection

Infeasible problems are tested (4 tests in `test_error_infeasible.py`). Unbounded problems have no test.

**Risk**: When no upper bound constrains a MAXIMIZE objective, the solver returns UNBOUNDED status. PackDB may not handle this status code correctly — could crash, hang, or return garbage values rather than raising a clear error. Infeasible and Unbounded are symmetric categories; missing the Unbounded path is a notable gap.

```sql
-- Unbounded: no upper bound on x
SELECT id, x FROM data
DECIDE x IS INTEGER
SUCH THAT x >= 1
MAXIMIZE SUM(x)

-- Unbounded: REAL variable with no upper bound
SELECT id, x FROM data
DECIDE x IS REAL
SUCH THAT x >= 0
MAXIMIZE SUM(x)
```

### MEDIUM: PER on per-row constraint rejection

Documented restriction: "PER requires an aggregate constraint." No test verifies the error message when `x <= 5 PER col` is attempted. Users could accidentally write this; the error message quality matters for usability.

```sql
-- Should be rejected with clear error
SUCH THAT x <= 5 PER grp
```

### LOW: HiGHS-specific error message quality

The `_expect_gurobi` decorator in `test_bilinear.py` and `test_quadratic.py` catches any `PackDBCliError` containing "quadratic" or "Gurobi" and passes the test. It does not verify the error message quality for non-convex bilinear objectives or MIQP with HiGHS. There is no dedicated test that runs HiGHS and checks the specific error message content.

```python
# Pseudo: dedicated HiGHS-only error checks
@pytest.mark.requires_highs_only
def test_highs_nonconvex_qp_error_message():
    with pytest.raises(PackDBCliError, match=r"non-convex.*Gurobi"):
        packdb.execute("... MAXIMIZE SUM(POWER(x, 2)) ...")

@pytest.mark.requires_highs_only
def test_highs_miqp_error_message():
    with pytest.raises(PackDBCliError, match=r"MIQP|integer.*quadratic.*Gurobi"):
        packdb.execute("... DECIDE keep IS BOOLEAN, x IS REAL ... MINIMIZE SUM(POWER(x-t, 2)) ...")
```

### LOW: `IN` on aggregate error message accuracy

`SUM(x) IN (...)` is rejected (`test_sum_with_in_not_allowed`), but no test verifies the exact error message matches the documented restriction.

### LOW: BETWEEN non-scalar error message accuracy

`test_between_non_scalar` expects a rejection but the match string could be more specific. A tighter regex on the error message would catch regressions in error messaging quality.
