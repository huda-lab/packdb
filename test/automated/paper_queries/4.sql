DROP VIEW IF EXISTS Tpch;
CREATE VIEW Tpch AS SELECT *, ps_supplycost AS ps_min_supplycost FROM part JOIN partsupp ON p_partkey = ps_partkey LIMIT 100;

SELECT *
FROM Tpch R
DECIDE x IS INTEGER
SUCH THAT x BETWEEN 0 AND 1 AND
    SUM(p_size*x) <= 8 AND
    SUM(x) >= 1
MINIMIZE SUM(ps_min_supplycost*x);