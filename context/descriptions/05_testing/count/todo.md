# COUNT Aggregate Test Coverage — Todo

## Missing coverage

### MEDIUM: MINIMIZE COUNT(x INTEGER)

Only `MAXIMIZE COUNT(x)` is tested for INTEGER. The indicator-variable direction matters for minimization — the solver wants to drive `z` values to 0, which then drives `x` values to 0 only if the constraint `z <= x` is correctly oriented.

```sql
-- MINIMIZE COUNT INTEGER
SELECT id, x FROM data
DECIDE x
SUCH THAT SUM(x) >= 20 AND x <= 10
MINIMIZE COUNT(x)
```
