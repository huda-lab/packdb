-- =====================================================================
-- CONSTRAINT SHAPES — stress-test queries against TPC-H
-- Each query is preceded by a comment block describing which branch it covers.
-- =====================================================================

-- --- C1: Per-row linear constraint ------------------------------------
-- Branch: per-row constraint, linear, with table/column data coefficients.
-- Scenario: decide how many of each part to order; per-row cap on quantity.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_retailprice < 1500
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
MAXIMIZE SUM(p_retailprice * qty);

-- --- C2: Aggregate SUM constraint -------------------------------------
-- Branch: aggregate SUM(dec_var) constraint with a scalar RHS.
-- Scenario: supplier selection with a total-count budget.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 20
MAXIMIZE SUM(s_acctbal * pick);

-- --- C3: Aggregate AVG constraint -------------------------------------
-- Branch: AVG(dec_var) rewritten to SUM with RHS scaling.
-- Scenario: limit average quantity picked across chosen parts.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 10
DECIDE qty IS INTEGER
SUCH THAT qty <= 20
  AND AVG(qty) <= 5
MAXIMIZE SUM(p_retailprice * qty);

-- --- C4: MAX(expr) <= K (easy, no Big-M) ------------------------------
-- Branch: MAX aggregate, easy direction, stripped to per-row bound.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MAX(s_acctbal * pick) <= 8000
  AND SUM(pick) >= 5
MAXIMIZE SUM(pick);

-- --- C5: MIN(expr) >= K (easy, no Big-M) ------------------------------
-- Branch: MIN aggregate, easy direction, stripped to per-row bound.
SELECT s_suppkey, s_acctbal, level
FROM supplier
WHERE s_acctbal > 0
DECIDE level IS INTEGER
SUCH THAT level <= 5
  AND MIN(s_acctbal * level) >= 100
MAXIMIZE SUM(level);

-- --- C6: MIN(expr) <= K (hard, Big-M indicators) ----------------------
-- Branch: MIN aggregate, hard direction, binary indicator per row.
-- Verified 2026-05-07: now rejected cleanly with
--   "Invalid Input Error: DECIDE expression contains an unsupported product
--    factor that still references decision variables after normalization..."
-- (Earlier crash / "Failed to add constraint to Gurobi" no longer reproduces.)
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MIN(s_acctbal * pick + 1000 * (1 - pick)) <= 500
  AND SUM(pick) >= 3
MAXIMIZE SUM(s_acctbal * pick);

-- --- C7: MAX(expr) >= K (hard, Big-M indicators) ----------------------
-- Branch: MAX aggregate, hard direction.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MAX(s_acctbal * pick) >= 9000
  AND SUM(pick) <= 10
MAXIMIZE SUM(pick);

-- --- C8: Aggregate equality (hard) ------------------------------------
-- Branch: SUM(...) = K.
SELECT n_nationkey, flag
FROM nation
DECIDE flag IS BOOLEAN
SUCH THAT SUM(flag) = 7
MAXIMIZE SUM(n_nationkey * flag);

-- --- C9: Composed MIN with SUM inside ---------------------------------
-- Branch: composed aggregate — MIN of per-row expression + SUM constraint.
-- Verified 2026-05-07: now rejected cleanly with the same Invalid Input
-- Error class as C6 ("unsupported product factor ..."). Earlier
-- "Failed to add constraint to Gurobi" no longer reproduces.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MAX(pick) <= 1
  AND SUM(pick) = 10
  AND MIN(s_acctbal * pick + 100000 * (1 - pick)) >= 1000
MAXIMIZE SUM(s_acctbal * pick);

-- --- C10: Quadratic constraint POWER(expr, 2) (QCQP, Gurobi only) -----
-- Branch: quadratic in SUCH THAT.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size BETWEEN 1 AND 5
DECIDE qty IS REAL
SUCH THAT qty <= 10
  AND SUM(POWER(qty, 2)) <= 500
MAXIMIZE SUM(p_retailprice * qty);

-- --- C11: Bilinear constraint — Boolean × anything (McCormick) --------
-- Branch: bool * real/int in SUCH THAT, linearized with McCormick.
SELECT p_partkey, p_retailprice, pick, qty
FROM part
WHERE p_size < 5
DECIDE pick IS BOOLEAN, qty IS INTEGER
SUCH THAT qty <= 20
  AND SUM(pick * qty) <= 50
MAXIMIZE SUM(p_retailprice * pick * qty);

-- --- C12: Bilinear constraint — general x*y (Gurobi only) -------------
-- Branch: int * real bilinear in SUCH THAT.
SELECT p_partkey, p_retailprice, qty, disc
FROM part
WHERE p_size < 3
DECIDE qty IS INTEGER, disc IS REAL
SUCH THAT qty <= 20 AND disc <= 5
  AND SUM(qty * disc) <= 100
MAXIMIZE SUM(p_retailprice * qty - qty * disc);

-- --- C13: Uncorrelated scalar subquery in constraint ------------------
-- Branch: scalar subquery RHS, bound once.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(s_acctbal * pick) <= (SELECT AVG(s_acctbal) * 20 FROM supplier)
MAXIMIZE SUM(pick);

-- --- C14: Correlated scalar subquery in constraint --------------------
-- Branch: per-row correlated subquery RHS.
-- BUG: "Failed to add constraint to Gurobi" when the correlated subquery
--      result is multiplied by a decision variable on both sides.
--      Needs a minimal repro; simpler correlated-subquery shapes may work.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT s_acctbal * pick >= (
    SELECT MIN(s2.s_acctbal) FROM supplier s2 WHERE s2.s_nationkey = supplier.s_nationkey
  ) * pick
  AND SUM(pick) >= 5
MAXIMIZE SUM(pick);

-- --- C15: BETWEEN constraint ------------------------------------------
-- Branch: BETWEEN desugars to two comparison constraints.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty BETWEEN 2 AND 9
MAXIMIZE SUM(qty);

-- --- C16: IN on column values (filter only) ---------------------------
-- Branch: IN on data column (not decision var).
SELECT n_nationkey, n_name, pick
FROM nation
WHERE n_regionkey IN (0, 1, 3)
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 10
MAXIMIZE SUM(pick);

-- --- C17: IN on decision variable -------------------------------------
-- Branch: dec_var IN (v1,v2,...) → auxiliary binary indicators.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty IN (0, 2, 5, 9)
  AND SUM(qty) >= 200
MAXIMIZE SUM(qty);

-- --- C18: <> (not equal) on decision variable -------------------------
-- Branch: Big-M disjunction, integer LHS required.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND qty <> 7
MAXIMIZE SUM(qty);

-- --- C19: Strict < on decision variable -------------------------------
-- Branch: strict inequality, requires integer-valued LHS.
SELECT n_nationkey, v
FROM nation
DECIDE v IS INTEGER
SUCH THAT v < 5
MAXIMIZE SUM(v);

-- --- C20: Strict > on decision variable -------------------------------
-- Branch: strict inequality >.
SELECT n_nationkey, v
FROM nation
DECIDE v IS INTEGER
SUCH THAT v > 2
  AND v <= 10
MAXIMIZE SUM(v);

-- --- C21: ABS in per-row constraint (lower-envelope) ------------------
-- Branch: ABS linearization for constraint LHS, |expr| <= K.
SELECT s_suppkey, s_acctbal, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 20
  AND ABS(qty - 5) <= 3
MAXIMIZE SUM(s_acctbal * qty);

-- --- C22: MIN(ABS(expr)) constraint (lower-envelope, easy direction) --
-- Branch: ABS inside MIN aggregate, MIN(...) >= K stripped to per-row.
SELECT s_suppkey, s_acctbal, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND MIN(ABS(qty - 4)) >= 1
MAXIMIZE SUM(s_acctbal * qty);

-- --- C23: MAX(ABS(expr)) constraint (Big-M) ---------------------------
-- Branch: ABS inside MAX aggregate, hard direction MAX(...) >= K.
SELECT s_suppkey, s_acctbal, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND MAX(ABS(qty - 5)) >= 4
  AND SUM(qty) >= 50
MAXIMIZE SUM(s_acctbal * qty);

-- --- C24: Bool × Bool bilinear constraint (AND-linearization) ---------
-- Branch: distinct from McCormick — uses simpler AND linearization.
SELECT s_suppkey, s_acctbal, pick, premium
FROM supplier
DECIDE pick IS BOOLEAN, premium IS BOOLEAN
SUCH THAT SUM(pick) <= 20
  AND SUM(pick * premium) <= 5
MAXIMIZE SUM(s_acctbal * pick * premium);

-- --- C25: Strict > on aggregate ---------------------------------------
-- Branch: SUM(...) > K, integer-valued LHS.
SELECT n_nationkey, pick
FROM nation
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) > 5
MAXIMIZE SUM(n_nationkey * pick);

-- --- C26: Strict < on aggregate ---------------------------------------
-- Branch: SUM(...) < K.
SELECT n_nationkey, pick
FROM nation
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) < 10
MAXIMIZE SUM(n_nationkey * pick);

-- --- C27: <> on aggregate ---------------------------------------------
-- Branch: SUM(...) <> K, Big-M disjunction on aggregate.
SELECT n_nationkey, pick
FROM nation
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 12
  AND SUM(pick) <> 7
MAXIMIZE SUM(n_nationkey * pick);

-- --- C28: BETWEEN on aggregate ----------------------------------------
-- Branch: SUM(...) BETWEEN K1 AND K2 desugars to two aggregate bounds.
SELECT s_suppkey, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) BETWEEN 5 AND 12
MAXIMIZE SUM(s_acctbal * pick);

-- --- C29: Per-row equality on decision variable -----------------------
-- Branch: per-row qty = K — fully pins variables that match.
SELECT s_suppkey, s_nationkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND qty = 7 WHEN (s_nationkey <= 5)
MAXIMIZE SUM(qty);
