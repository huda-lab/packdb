# ABS Linearization Test Coverage — Todo

## Missing coverage

### HIGH: ABS in aggregate constraint with WHEN

`SUM(ABS(expr)) op K WHEN condition` — ABS+WHEN is tested on objectives but not on aggregate constraints. The linearization creates auxiliary variables with unconditional linking constraints (`d >= expr`, `d >= -expr`). When WHEN is present on the aggregate, the auxiliary variable's contribution to `SUM(d)` should include only WHEN-matching rows. If the WHEN filter doesn't propagate to the auxiliary variable's coefficient in the aggregate, wrong rows contribute to the sum.

```sql
-- ABS aggregate constraint with WHEN
WITH data AS (
    SELECT 1 AS id, 10.0 AS target, true AS active, 5.0 AS profit UNION ALL
    SELECT 2, 20.0, false, 8.0 UNION ALL
    SELECT 3, 15.0, true, 3.0
)
SELECT id, ROUND(x, 2) AS x, target, active
FROM data
DECIDE x IS REAL
SUCH THAT x <= 30 AND SUM(ABS(x - target)) <= 8 WHEN active
MAXIMIZE SUM(x * profit)
```

_(PER + ABS aggregate constraint covered in `test_per_interactions.py::test_per_abs_aggregate` — Batch 1, 2026-04-15.)_
