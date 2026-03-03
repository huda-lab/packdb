-- Row-wise bounds: x <= 5 for all rows, plus global sum
SELECT ps_partkey, ps_availqty, x
FROM partsupp
WHERE ps_partkey < 20
DECIDE x
SUCH THAT x IS INTEGER
  AND x <= 5
  AND SUM(x) <= 100
MAXIMIZE SUM(x * ps_availqty);
