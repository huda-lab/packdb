-- Subquery RHS: Total price < Avg customer acctbal
SELECT o_orderkey, o_totalprice, x
FROM orders
WHERE o_orderkey < 100
DECIDE x
SUCH THAT x IS BINARY
  AND SUM(x * o_totalprice) <= (SELECT AVG(c_acctbal) FROM customer WHERE c_nationkey = 10)
MAXIMIZE SUM(x * o_totalprice);
