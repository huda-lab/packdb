DROP VIEW IF EXISTS Tpch;
CREATE VIEW Tpch AS SELECT *, l_extendedprice AS revenue FROM lineitem LIMIT 100;

SELECT *
FROM Tpch R
DECIDE x IS INTEGER
SUCH THAT x BETWEEN 0 AND 1 AND
    SUM(revenue*x) >= 73000 AND
    SUM(l_quantity*x) <= 110.95 AND
    SUM(x) >= 1
MINIMIZE SUM(x);