SELECT SUM(x), SUM(y), l_orderkey FROM lineitem WHERE l_extendedprice > 4 
DECIDE x, y SUCH THAT SUM(x*l_tax) <= 3 AND x IS BINARY MAXIMIZE SUM(x*l_discount)
GROUP BY l_orderkey HAVING SUM(l_quantity) > 0
LIMIT 5;