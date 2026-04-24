SELECT l_orderkey, l_linenumber, l_quantity, l_discount, qty
FROM lineitem
DECIDE qty IS REAL
SUCH THAT qty >= 0
    AND qty <= 50
    AND SUM(POWER(qty, 2)) <= ${Q9_SSE_CAP}
MINIMIZE SUM(POWER(qty - l_quantity, 2));
