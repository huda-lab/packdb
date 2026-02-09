-- Integer Procurement: Buy parts, limit by availqty, budget constraint
SELECT ps_partkey, ps_suppkey, ps_supplycost, ps_availqty, x
FROM partsupp
WHERE ps_partkey < 50
DECIDE x IS INTEGER
SUCH THAT x <= ps_availqty
  AND SUM(x * ps_supplycost) <= 10000
MAXIMIZE SUM(x);
