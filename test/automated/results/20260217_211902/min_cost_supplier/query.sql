SELECT x, ps_partkey, ps_suppkey, ps_supplycost, ps_availqty
FROM partsupp
WHERE ps_partkey <= 50
DECIDE x IS BOOLEAN
SUCH THAT
    SUM(x * ps_availqty) >= 1000
MINIMIZE SUM(x * ps_supplycost);
