-- Multi Constraint: Weight (quantity) and Volume (size)
SELECT l.l_orderkey, l.l_quantity, p.p_size, x
FROM lineitem l, part p
WHERE l.l_partkey = p.p_partkey
  AND l.l_orderkey < 50
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_quantity) <= 500
  AND SUM(x * p.p_size) <= 1000
MAXIMIZE SUM(x);
