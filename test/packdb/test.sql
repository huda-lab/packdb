SELECT SUM(x), SUM(y), l_orderkey FROM lineitem WHERE l_extendedprice > 4 
DECIDE x, y SUCH THAT 
    SUM(3*x*l_tax) <= 6 
    AND SUM(-2*y*l_quantity) >= -4 
    AND x IS BINARY 
MAXIMIZE SUM(5*x*l_discount + 10*y*l_extendedprice)
GROUP BY l_orderkey HAVING SUM(l_quantity) > 0
LIMIT 5;