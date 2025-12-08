-- Complex Coefficients: Discounted price calculation
SELECT l_orderkey, l_extendedprice, l_discount, l_tax, x
FROM lineitem
WHERE l_orderkey < 50
DECIDE x
SUCH THAT x IS BINARY
  AND SUM(x * (l_extendedprice * (1 - l_discount) * (1 + l_tax))) <= 50000
MAXIMIZE SUM(x);
