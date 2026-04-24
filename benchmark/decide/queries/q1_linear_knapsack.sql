SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, x
FROM lineitem
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_quantity) <= ${Q1_QTY_CAP}
MAXIMIZE SUM(x * l_extendedprice);
