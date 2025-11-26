-- SELECT SUM(x), SUM(y), l_orderkey FROM lineitem WHERE l_extendedprice > 4 
-- DECIDE x, y 
-- SUCH THAT 
--     SUM(3*x*l_tax + (l_quantity+10)^2) <= 6 
--     AND SUM(2*y*l_tax - 2*y*l_quantity) + 5 >= -4 
--     AND x IS BINARY 
-- MAXIMIZE SUM(5*x*l_discount - 10*y*l_extendedprice + 10 + l_extendedprice)
-- GROUP BY l_orderkey HAVING SUM(l_quantity) > 0
-- LIMIT 5;



-- -- Test with smaller dataset (25 rows instead of 6M)
-- -- Testing strict inequality operators: < and >
-- -- Expected: x should be 3, 4, or 5 (integers in range 2 < x < 6)
SELECT x,y, l_extendedprice, l_tax, l_orderkey, l_linenumber
FROM lineitem
WHERE l_orderkey <= 10  -- Limit to ~25 rows
DECIDE x, y
SUCH THAT
    SUM(x) <= 1000
    AND x > 2             -- Strict: > (so x >= 3)
    AND x < 6             -- Strict: < (so x <= 5)
    AND y < 10
    AND y > 2
MAXIMIZE SUM(x*l_extendedprice - y*l_tax);



-- SELECT SUM(x) AS total_x
-- FROM lineitem
-- DECIDE x
-- SUCH THAT SUM(3*x*l_tax + (l_quantity + 7)^2) <= 25
-- MAXIMIZE SUM(x*l_discount);


-- SELECT SUM(x) AS total_x
-- FROM lineitem
-- DECIDE x
-- SUCH THAT SUM(5*x*l_tax) + 3 <= 42
-- MAXIMIZE SUM(x*l_extendedprice);


-- SELECT SUM(x) AS total_x
-- FROM lineitem
-- DECIDE x
-- SUCH THAT SUM(x*l_tax) <= 100
-- MAXIMIZE SUM(x*l_quantity);


-- SELECT SUM(x) AS total_x
-- FROM lineitem
-- DECIDE x
-- SUCH THAT SUM(x*l_tax - l_quantity) <= 50
-- MAXIMIZE SUM(x*l_extendedprice);


-- SELECT SUM(x) AS total_x
-- FROM lineitem
-- DECIDE x
-- SUCH THAT SUM((l_quantity + 4) * (x*l_tax + 3)) <= 120
-- MAXIMIZE SUM(x*l_discount);


-- SELECT SUM(x) AS total_x
-- FROM lineitem
-- DECIDE x
-- SUCH THAT SUM(x*l_tax) + (l_quantity + 2) <= 30
-- AND SUM(4*x*l_discount) - 5 >= 0
-- MAXIMIZE SUM(x*l_extendedprice);


-- SELECT SUM(x) AS total_x
-- FROM lineitem
-- DECIDE x
-- SUCH THAT SUM(x*l_tax + l_quantity*l_quantity + 1) <= 50
-- AND SUM(2*x*l_discount + (l_extendedprice + 3)^2) >= 10
-- MAXIMIZE SUM(x*l_discount);


-- SELECT SUM(x), SUM(y)
-- FROM lineitem
-- DECIDE x, y
-- SUCH THAT SUM(3*x*l_tax + 2*y*l_discount + (l_quantity - 2)^2 + 7) <= 35
-- MAXIMIZE SUM(x*l_extendedprice - y*l_tax);


