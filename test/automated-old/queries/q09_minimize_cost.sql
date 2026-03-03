-- Minimize Cost: Select suppliers to fulfill demand (count >= 10)
SELECT s_suppkey, s_acctbal, x
FROM supplier
WHERE s_nationkey = 5
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 10
MINIMIZE SUM(x * s_acctbal);
