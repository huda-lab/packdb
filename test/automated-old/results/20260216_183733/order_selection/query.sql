SELECT x, o_orderkey, o_totalprice
FROM orders
WHERE o_orderdate >= '1995-01-01' AND o_orderdate < '1995-02-01'
DECIDE x IS BOOLEAN
SUCH THAT
    SUM(x) <= 300
MAXIMIZE SUM(x * o_totalprice);
