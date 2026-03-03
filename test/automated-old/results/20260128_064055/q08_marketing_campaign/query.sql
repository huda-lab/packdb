-- Marketing: Select customers, cost = 10 per customer, budget 500
SELECT c_custkey, c_acctbal, x
FROM customer
WHERE c_nationkey = 1
DECIDE x
SUCH THAT x IS BINARY
  AND SUM(x * 10) <= 500
MAXIMIZE SUM(x * c_acctbal);
