SELECT x, o_orderkey, o_totalprice
FROM orders
WHERE o_orderdate >= '1995-01-01' AND o_orderdate < '1995-02-01'
DECIDE x
SUCH THAT
    SUM(x) <= 10
    AND x IS BINARY
MAXIMIZE SUM(x * o_totalprice);
