SELECT l_orderkey, l_linenumber, l_quantity, new_qty
FROM lineitem
DECIDE new_qty IS REAL
SUCH THAT SUM(ABS(new_qty - l_quantity)) <= ${Q2_ABS_CAP}
    AND MAX(new_qty) >= 40
    AND new_qty <= 50
MINIMIZE SUM(ABS(new_qty - l_quantity));
