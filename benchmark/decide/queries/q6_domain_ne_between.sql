SELECT o_orderkey, o_totalprice, ship_bins
FROM orders
DECIDE ship_bins IS INTEGER
SUCH THAT ship_bins IN (0, 1, 3)
    AND ship_bins BETWEEN 0 AND 3
    AND SUM(ship_bins) <> ${Q6_NE_SUM}
    AND SUM(ship_bins * o_totalprice) <= ${Q6_PRICE_CAP}
MAXIMIZE SUM(ship_bins * o_totalprice);
