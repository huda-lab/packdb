SELECT l.l_orderkey, l.l_linenumber, o.o_orderpriority, l.l_quantity, l.l_extendedprice, keep_order
FROM lineitem l JOIN orders o ON l.l_orderkey = o.o_orderkey
DECIDE o.keep_order IS BOOLEAN
SUCH THAT SUM(keep_order * l.l_quantity) <= ${Q7_QTY_CAP}
    AND SUM(keep_order) <= ${Q7_PRIORITY_CAP} PER o_orderpriority
MAXIMIZE SUM(keep_order * l.l_extendedprice);
