DROP VIEW IF EXISTS tpch;
CREATE VIEW tpch AS SELECT *, l_quantity AS quantity, l_extendedprice AS price, l_tax AS tax FROM lineitem LIMIT 100;

SELECT *
FROM tpch R
DECIDE x IS INTEGER
SUCH THAT x BETWEEN 0 AND 1 AND
    SUM(x) BETWEEN 50 AND 150 AND
    SUM(quantity*x) <= 100 AND
    SUM(price*x) BETWEEN 100 AND 2000
MINIMIZE SUM(tax*x);