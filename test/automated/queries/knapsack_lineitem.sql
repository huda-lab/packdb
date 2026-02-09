SELECT x, l_orderkey, l_linenumber, l_extendedprice, l_quantity
FROM lineitem
WHERE l_orderkey <= 100
DECIDE x IS BOOLEAN
SUCH THAT
    SUM(x * l_quantity) <= 500
MAXIMIZE SUM(x * l_extendedprice);
