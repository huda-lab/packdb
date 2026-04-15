# Bilinear Term Test Coverage — Todo

## Missing coverage

### HIGH: PER + bilinear

Explicitly documented as untested in `03_expressivity/bilinear/done.md`.

**Risk**: McCormick envelope linearization creates auxiliary variables `w` and Big-M constraints (`w <= M*b`, `w <= x`, `w >= x - M*(1-b)`, `w >= 0`). With PER, these auxiliary constraints must be partitioned by group — the McCormick constraints for group A's rows must not constrain group B's rows. If McCormick constraints are generated globally instead of per-group, the feasible region is wrong.

```sql
-- Bilinear objective with PER
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, 3.0 AS profit UNION ALL
    SELECT 2, 'A', 1.0 UNION ALL
    SELECT 3, 'B', 5.0 UNION ALL
    SELECT 4, 'B', 2.0
)
SELECT id, grp, b, ROUND(x, 2) AS x
FROM data
DECIDE b IS BOOLEAN, x IS REAL
SUCH THAT x <= 10 AND SUM(b * x) <= 15 PER grp
MAXIMIZE SUM(profit * b * x)
```

### HIGH: bilinear + WHEN + PER triple interaction

Both bilinear+PER (above) and the triple combination are untested. All three systems must compose correctly: WHEN filters rows, PER partitions the remaining rows into groups, and McCormick constraints must respect both.

```sql
-- Triple: bilinear + WHEN + PER
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, true AS active, 5.0 AS profit UNION ALL
    SELECT 2, 'A', false, 3.0 UNION ALL
    SELECT 3, 'B', true, 8.0 UNION ALL
    SELECT 4, 'B', true, 2.0
)
SELECT id, grp, b, ROUND(x, 2) AS x
FROM data
DECIDE b IS BOOLEAN, x IS REAL
SUCH THAT x <= 10 AND SUM(b * x) <= 12 WHEN active PER grp
MAXIMIZE SUM(b * x * profit)
```

### HIGH: entity_scope + bilinear

No test combines table-scoped variables with bilinear terms.

**Risk**: Entity deduplication means multiple result rows share one solver variable. McCormick auxiliary variable indexing must use entity-keyed indices, not row indices. A mismatch produces wrong linking constraints.

```sql
-- Entity-scoped variable with bilinear
SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN, ROUND(x, 2) AS x
FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
WHERE n.n_regionkey = 0 AND n.n_nationkey <= 5
DECIDE n.keepN IS BOOLEAN, x IS REAL
SUCH THAT x <= 100 AND SUM(keepN * x) <= 500
MAXIMIZE SUM(keepN * x * c_acctbal)
```

### MEDIUM: Bilinear in MINIMIZE objective

All bilinear objective tests use MAXIMIZE. The sign of the Q matrix off-diagonal entries matters for MINIMIZE — if the sign is wrong, MINIMIZE becomes MAXIMIZE for the bilinear component and vice versa.

```sql
-- Bilinear MINIMIZE (both solvers for Bool × Real)
SELECT id, b, ROUND(x, 2) AS x FROM data
DECIDE b IS BOOLEAN, x IS REAL
SUCH THAT x <= 10 AND SUM(b) >= 2
MINIMIZE SUM(b * x * cost)

-- Non-convex MINIMIZE (Gurobi only)
DECIDE x IS REAL, y IS REAL
SUCH THAT x <= 10 AND y <= 10
MINIMIZE SUM(x * y)
```

### LOW: McCormick envelope feasible region verification (Bool × Real in constraint)

The bilinear *constraint* test in `test_bilinear.py` covers Bool × Bool. No test verifies that McCormick envelope constraints produce correct feasible regions specifically for Bool × Real constraints (as opposed to objectives). Both should work via the same McCormick machinery, but confirmation is valuable.

```sql
-- Bilinear Bool × Real in constraint
SELECT id, b, ROUND(x, 2) AS x FROM data
DECIDE b IS BOOLEAN, x IS REAL
SUCH THAT x <= 10 AND SUM(b * x) <= 25
MAXIMIZE SUM(x)
```
