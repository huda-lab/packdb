SELECT o_orderkey, o_totalprice, choose_order, expedite_order
FROM (
    SELECT o_orderkey, o_totalprice
    FROM orders
    ORDER BY o_orderkey
    LIMIT ${Q8_ROW_LIMIT}
) orders
DECIDE choose_order IS BOOLEAN, expedite_order IS BOOLEAN
SUCH THAT SUM(choose_order) <= ${Q8_CHOOSE_CAP}
    AND SUM(expedite_order) <= ${Q8_EXPEDITE_CAP}
    AND SUM(choose_order * expedite_order * o_totalprice) <= ${Q8_PAIR_PRICE_CAP}
MAXIMIZE SUM(choose_order * o_totalprice + expedite_order * 0.1 * o_totalprice + choose_order * expedite_order * o_totalprice);
