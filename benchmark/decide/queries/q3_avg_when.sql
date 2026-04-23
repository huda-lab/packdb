SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
       l_discount, l_returnflag, x
FROM lineitem
DECIDE x
SUCH THAT x <= 5
    AND AVG(x * l_discount) <= 0.25
    AND SUM(x * l_quantity) <= 200 WHEN l_returnflag = 'R'
MAXIMIZE SUM(x * l_extendedprice);
