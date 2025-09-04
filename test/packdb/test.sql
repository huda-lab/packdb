SELECT x, y, l_orderkey FROM lineitem WHERE l_extendedprice > 4 
DECIDE x, y SUCH THAT SUM(x*l_tax) <= 3 AND x IS BINARY MAXIMIZE SUM(x*l_discount)
LIMIT 5;

-- SELECT l_orderkey, SUM(l_quantity) FROM lineitem WHERE l_extendedprice > 4 AND l_linenumber > 1
-- GROUP BY l_orderkey HAVING SUM(l_tax) > 0.1 LIMIT 10;