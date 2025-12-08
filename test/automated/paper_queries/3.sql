DROP VIEW IF EXISTS Tpch;
CREATE VIEW Tpch AS SELECT *, l_extendedprice AS sum_base_price, l_extendedprice AS sum_disc_price, l_extendedprice AS sum_charge, l_quantity AS sum_avg_qty, l_extendedprice AS sum_avg_price, l_discount AS sum_avg_disc, l_quantity AS sum_sum_qty, l_linenumber AS count_order FROM lineitem LIMIT 100;

SELECT *
FROM Tpch R
DECIDE x
SUCH THAT x IS INTEGER AND
    x BETWEEN 0 AND 1 AND
    SUM(sum_base_price*x) <= 15000000 AND
    SUM(sum_disc_price*x) <= 45000000 AND
    SUM(sum_charge*x) <= 96000000 AND
    SUM(sum_avg_qty*x) <= 50.36 AND
    SUM(sum_avg_price*x) <= 69000 AND
    SUM(sum_avg_disc*x) <= 0.11 AND
    SUM(sum_sum_qty*x) <= 78000 AND
    SUM(x) >= 1
MAXIMIZE SUM(count_order*x)