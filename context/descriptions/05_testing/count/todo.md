# COUNT Aggregate Test Coverage — Todo

## Missing coverage

### HIGH: COUNT(x INTEGER) with PER

`test_count_with_per` uses BOOLEAN variables, where COUNT trivially rewrites to SUM. The INTEGER path (Big-M indicator variable creation + PER group partitioning) has no test.

**Risk**: The Big-M linking constraints (`z <= x`, `x <= M*z`) must be generated per-group or globally depending on the constraint structure. Wrong scoping produces incorrect COUNT semantics per group (e.g., a non-zero `x` in group A might erroneously set `z` in group B to 1, or vice versa).

```sql
-- COUNT INTEGER with PER
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, 10 AS val UNION ALL
    SELECT 2, 'A', 5 UNION ALL
    SELECT 3, 'B', 8 UNION ALL
    SELECT 4, 'B', 3
)
SELECT id, grp, x FROM data
DECIDE x
SUCH THAT x <= 10 AND COUNT(x) >= 1 PER grp
MAXIMIZE SUM(x * val)
```

### MEDIUM: MINIMIZE COUNT(x INTEGER)

Only `MAXIMIZE COUNT(x)` is tested for INTEGER. The indicator-variable direction matters for minimization — the solver wants to drive `z` values to 0, which then drives `x` values to 0 only if the constraint `z <= x` is correctly oriented.

```sql
-- MINIMIZE COUNT INTEGER
SELECT id, x FROM data
DECIDE x
SUCH THAT SUM(x) >= 20 AND x <= 10
MINIMIZE COUNT(x)
```
