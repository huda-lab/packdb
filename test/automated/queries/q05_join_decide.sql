-- Join: Select orders from BUILDING segment
SELECT o.o_orderkey, o.o_totalprice, c.c_mktsegment, x
FROM orders o, customer c
WHERE o.o_custkey = c.c_custkey
  AND c.c_mktsegment = 'BUILDING'
  AND o.o_orderkey < 1000
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * o.o_totalprice) <= 100000
MAXIMIZE SUM(x * o.o_totalprice);
