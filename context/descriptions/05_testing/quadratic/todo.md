# Quadratic Programming Test Coverage — Todo

## Missing coverage

### HIGH: QP objective with PER

Quadratic *constraints* with PER are tested; quadratic *objectives* with PER have zero coverage.

**Risk**: Q matrix construction with PER group auxiliaries. With nested PER objectives (`SUM(SUM(POWER(x - t, 2))) PER col`), inner creates per-group Q matrices, outer creates a global sum across groups. A bug here produces wrong optimal values without raising any error.

```sql
-- QP objective with PER
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
    SELECT 2, 'A', 15.0 UNION ALL
    SELECT 3, 'B', 20.0 UNION ALL
    SELECT 4, 'B', 25.0
)
SELECT id, grp, ROUND(x, 4) AS x
FROM data
DECIDE x IS REAL
SUCH THAT x >= 0 AND x <= 100 AND SUM(x) >= 5 PER grp
MINIMIZE SUM(POWER(x - target, 2))

-- Explicit nested form
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
