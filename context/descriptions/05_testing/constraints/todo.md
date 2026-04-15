# Constraint Operator Test Coverage — Todo

## Known bug to fix

`test_ne_aggregate_local_when_constraint` is **xfail** — NE Big-M expansion does not compose with aggregate-local WHEN. See [when/todo.md](../when/todo.md).

## Missing coverage

### HIGH: Strict `<` / `>` with IS REAL variables

No test uses strict inequality with continuous variables.

**Risk**: For integer variables, the oracle converts `SUM(x) < 100` to `SUM(x) <= 99`, which is mathematically equivalent. For REAL variables, this is **wrong** — `SUM(x) < 100` is a strict open constraint, not the same as `<= 99`. If PackDB also converts `<` to `<= val-1` internally for REAL, results are silently incorrect.

This gap may also indicate an oracle bug — the oracle's strict-inequality handling may need fixing before the test can be written.

```sql
-- Strict inequality with REAL
SELECT id, ROUND(x, 4) AS x FROM data
DECIDE x IS REAL
SUCH THAT x <= 10 AND SUM(x) < 25.5
MAXIMIZE SUM(x)
```

### MEDIUM: Aggregate BETWEEN without aggregate-local WHEN

`SUM(x) BETWEEN 10 AND 50` desugars to `SUM(x) >= 10 AND SUM(x) <= 50`. The only aggregate BETWEEN test is nested inside `test_aggregate_local_when.py`. The standalone desugaring path — where the binder produces two constraints from one BETWEEN expression on an aggregate — is untested. Wrong signs or RHS values on aggregate inputs would go unnoticed.

```sql
-- Standalone aggregate BETWEEN
SELECT id, x FROM items
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * weight) BETWEEN 10 AND 50
MAXIMIZE SUM(x * value)
```

### MEDIUM: Negative coefficients in aggregate constraints

Only negative *objective* coefficients are tested (`test_edge_cases.py`). `SUM(x * col) <= K` where `col` has negative values tests the sign handling during coefficient extraction in `physical_decide.cpp`. A sign error could silently flip constraint direction.

```sql
-- Negative coefficients in aggregate constraint
WITH data AS (
    SELECT 1 AS id, -5.0 AS cost, 10.0 AS val UNION ALL
    SELECT 2, -3.0, 8.0 UNION ALL
    SELECT 3, 7.0, 15.0 UNION ALL
    SELECT 4, -1.0, 5.0
)
SELECT id, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * cost) >= -10
MAXIMIZE SUM(x * val)
```

### LOW: `<>` on aggregate with coefficient-introduced sign complexity

`SUM(x) <> 0` is tested, but `SUM(x * col) <> 0` — where the coefficient column introduces sign complexity into the Big-M disjunction — is not.

### LOW: Negative constant multiplier in SUM

`SUM(x * (-5)) <= K` or `SUM(-x) <= K`. The symbolic normalizer in `decide_symbolic.cpp` handles sign normalization, but no explicit test covers a purely negative constant multiplier in an aggregate constraint. Sign normalization bugs would silently flip constraint polarity.

### LOW: `IN` on aggregate error message accuracy

`SUM(x) IN (...)` is rejected (`test_sum_with_in_not_allowed`), but no test verifies the *exact* error message matches the documented restriction — important for usability.

## Cross-references

- Subquery RHS variations — see [subquery/todo.md](../subquery/todo.md)
- Aggregate operator + PER interactions — see [per/todo.md](../per/todo.md)
