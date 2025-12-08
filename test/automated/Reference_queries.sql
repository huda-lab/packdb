SELECT PACKAGE(*) AS P FROM tpch R REPEAT 0
SUCH THAT 15 <= COUNT(P.*) <= 45
AND SUM(P.quantity) <= b1
AND SUM(P.discount) >= b2
AND SUM(P.tax) BETWEEN b3 AND b4
MAXIMIZE SUM(P.price);



SELECT PACKAGE(*) AS P FROM tpch R REPEAT 0
SUCH THAT 50 <= COUNT(P.*) <= 150
AND SUM(P.quantity) <= b1
AND SUM(P.price) BETWEEN b2 AND b3
MINIMIZE SUM(P.tax);



SELECT PACKAGE(*) AS P
FROM Tpch REPEAT 0
SUCH THAT SUM(sum_base_price) <= 15000000
  AND SUM(sum_disc_price) <= 45000000
  AND SUM(sum_charge) <= 96000000
  AND SUM(sum_avg_qty) <= 50.36
  AND SUM(sum_avg_price) <= 69000
  AND SUM(sum_avg_disc) <= 0.11
  AND SUM(sum_sum_qty) <= 78000
  AND COUNT(*) >= 1
MAXIMIZE SUM(count_order);


SELECT PACKAGE(*) AS P
FROM Tpch REPEAT 0
SUCH THAT SUM(p_size) <= 8
  AND COUNT(*) >= 1
MINIMIZE SUM(ps_min_supplycost);


SELECT PACKAGE(*) AS P
FROM Tpch REPEAT 0
SUCH THAT SUM(revenue) >= 414000
  AND COUNT(*) >= 1
MINIMIZE COUNT(*);




SELECT PACKAGE(*) AS P
FROM Tpch REPEAT 0
SUCH THAT SUM(o_totalprice) <= 454000
  AND SUM(o_shippriority) >= 0
  AND COUNT(*) >= 1
MINIMIZE COUNT(*)




SELECT PACKAGE(*) AS P
FROM Tpch REPEAT 0
SUCH THAT SUM(revenue) >= 720000
  AND COUNT(*) >= 1
MINIMIZE COUNT(*)





SELECT PACKAGE(*) AS P
FROM Tpch REPEAT 0
SUCH THAT SUM(revenue) >= 73000
  AND SUM(l_quantity) <= 110.95
  AND COUNT(*) >= 1
MINIMIZE COUNT(*)



SELECT PACKAGE(*) AS P
FROM Tpch REPEAT 0
SUCH THAT COUNT(*) <= 2.667e-6 * (SELECT COUNT(*) FROM Tpch)
  AND COUNT(*) >= 1
MAXIMIZE SUM(revenue)