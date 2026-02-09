-- Logic: Between constraint
SELECT l_orderkey, l_extendedprice, x
FROM lineitem
WHERE l_orderkey < 20
DECIDE x IS INTEGER
SUCH THAT x BETWEEN 0 AND 5
  AND SUM(x * l_extendedprice) <= 10000
MAXIMIZE SUM(x);
