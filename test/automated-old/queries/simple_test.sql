SELECT x, l_orderkey, l_linenumber, l_extendedprice, l_tax 
FROM LINEITEM 
WHERE l_orderkey <= 5
DECIDE x
SUCH THAT SUM(x * l_extendedprice) <= 10000
MAXIMIZE SUM(x * l_extendedprice);
