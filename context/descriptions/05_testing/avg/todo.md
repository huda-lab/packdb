# AVG Aggregate Test Coverage — Todo

## Open bugs

### MEDIUM: AVG with `<>` operator — rewrite distributes `1/N` into LHS coefficients

**Status:** xfail test in place (`test_avg.py::test_avg_not_equal_boolean`, `::test_avg_not_equal_with_when`). Both raise `PackDBCliError` with the integer-step guard message.

**Symptom:** Every `AVG(x) <> K` query is rejected:

```
Invalid Input Error: Inequality '<>' is not supported when the left-hand side
involves a REAL variable or a non-integer coefficient. The integer-step
rewrite (x <> K → x <= K-1 OR x >= K+1) would cut continuous feasible
points in the band (K-1, K+1).
```

**Root cause (empirical):** `AVG(x) <=`/`=` work correctly (oracle-verified by the other `test_avg_*` tests), but the NE path lowers `AVG(x) <> K` by distributing `1/N` into LHS coefficients rather than hoisting `N` to the RHS. The NE integer-step guard in `physical_decide.cpp` then sees fractional coefficients and rejects. Rewrite order: `RewriteNotEqual` (step 4 in `decide_optimizer.cpp:46-64`) runs before `RewriteAvgToSum` (step 6), so the NE indicator is created against the still-AVG LHS; by the time the guard runs, the AVG has been lowered into fractional coefficients.

**Fix direction:** the AVG→SUM rewrite must, for the NE case, emit `SUM(x) <> K*N` (integer-valued LHS) rather than `SUM((1/N)*x) <> K`. Un-xfail both tests when fixed.

**Example:**
```sql
SELECT id, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT AVG(x) <> 0.5
MAXIMIZE SUM(x * profit)
```
