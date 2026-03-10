DROP VIEW IF EXISTS tpch;
CREATE VIEW tpch AS SELECT *, l_quantity AS quantity, l_extendedprice AS price, l_discount AS discount, l_tax AS tax FROM lineitem LIMIT 100;

SELECT *
FROM tpch R
DECIDE x IS INTEGER
SUCH THAT x BETWEEN 0 AND 1 AND
    SUM(x) BETWEEN 15 AND 45 AND
    SUM(quantity*x) <= 100 AND
    SUM(discount*x) >= 30 AND
    SUM(tax*x) BETWEEN 100 AND 2000
MAXIMIZE SUM(price*x);