# Subquery Test Coverage — Todo

## Missing coverage

### MEDIUM: Correlated subquery with IS REAL variable

All correlated subquery tests use BOOLEAN or INTEGER. The decorrelation join produces per-row values; with REAL variables, these could be fractional bounds — the continuous-variable path interacting with decorrelation is untested.

```sql
-- Correlated subquery RHS with REAL variable
SELECT l.l_orderkey, l.l_linenumber, ROUND(x, 2) AS x
FROM lineitem l
DECIDE x IS REAL
SUCH THAT x <= (
    SELECT AVG(l2.l_quantity)
    FROM lineitem l2
    WHERE l2.l_orderkey = l.l_orderkey
)
MAXIMIZE SUM(x * l.l_extendedprice)
```

### LOW: Uncorrelated subquery in PER constraint RHS

`SUM(x) <= (SELECT AVG(col) FROM other_table) PER group` — the scalar RHS must be shared across all PER groups. If execution evaluates the subquery per-group, errors could occur. Only the error case (non-scalar rejection) is tested; a positive test is missing.

```sql
SELECT l_orderkey, l_linenumber, l_extendedprice, x
FROM lineitem WHERE l_orderkey <= 5
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_extendedprice) <= (SELECT 10000) PER l_orderkey
MAXIMIZE SUM(x)
```

### LOW: Subquery unexpectedly returning multiple rows

Uncorrelated scalar subqueries are tested (expecting exactly one row). No DECIDE-specific test verifies the error path when a subquery that is *expected* to be scalar unexpectedly returns more than one row. DuckDB's standard handling may cover this, but explicit validation is useful.

### LOW: `ExpressionContainsDecideVariable` error message accuracy

`test_subquery_rhs_non_scalar` currently matches `"not a scalar"`, which may be catching a different error than the intended "subquery references decide variable" check. A dedicated test matching the exact error message for `x <= (SELECT x + 1 FROM ...)` would be more precise.
