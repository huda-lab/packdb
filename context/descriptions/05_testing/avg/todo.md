# AVG Aggregate Test Coverage — Todo

## Missing coverage

### MEDIUM: AVG with `<>` operator

`AVG(x) <> K` desugars to `SUM(x) <> K*N` — scaled RHS interacts with the Big-M disjunction rewrite used for `<>`. If the RHS scaling happens after the `<>` rewrite, the disjunction bounds are wrong.

```sql
-- AVG with not-equal
SELECT id, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT AVG(x) <> 0.5
MAXIMIZE SUM(x * profit)
```

### LOW: PER group with exactly 1 row + AVG (scaling edge case)

Off-by-one or division issues in the `1/n_g` scaling when a PER group has a single row, combined with groups of very different sizes. AVG scaling stresses the coefficient scaling path differently when `n_g = 1` vs. `n_g = 50` in the same query.

```sql
-- Asymmetric PER groups with AVG
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, 10.0 AS val UNION ALL
    SELECT 2, 'B', 5.0 UNION ALL SELECT 3, 'B', 8.0 UNION ALL
    SELECT 4, 'B', 3.0 UNION ALL SELECT 5, 'B', 7.0
)  -- group A has 1 row, group B has 4
SELECT id, grp, val, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 1 PER grp
MINIMIZE SUM(AVG(x * val)) PER grp
```
