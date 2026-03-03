-- Binary Knapsack: Select lineitems to maximize extendedprice, total quantity <= 100
SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
FROM lineitem
WHERE l_orderkey < 100
DECIDE x
SUCH THAT x IS BINARY
  AND SUM(x * l_quantity) <= 100
MAXIMIZE SUM(x * l_extendedprice);
