SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, l_discount, x
FROM lineitem
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_quantity) <= ${Q5_QTY_CAP}
    AND SUM(x) <= ${Q5_COUNT_CAP}
    AND SUM(x * l_extendedprice * l_discount) <= ${Q5_DISCOUNT_CAP}
MAXIMIZE SUM(x * l_extendedprice * (1 - l_discount));
