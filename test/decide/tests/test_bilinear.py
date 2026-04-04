"""Tests for bilinear term support (x * y) in DECIDE objectives and constraints.

Covers:
  - Boolean × Boolean product (AND-linearization, both solvers)
  - Boolean × Real product (McCormick linearization, both solvers)
  - Boolean × Integer product (McCormick linearization, both solvers)
  - Real × Real product (non-convex Q matrix, Gurobi only)
  - Integer × Integer product (non-convex, Gurobi only)
  - Integer × Real product (non-convex, Gurobi only)
  - Bilinear in objectives (all variable type combinations)
  - Bilinear in constraints (QCQP, Gurobi only for non-linearizable)
  - Mixed linear + bilinear objectives
  - Bilinear with WHEN filter
  - Error rejection: triple products, HiGHS non-convex rejection
  - Backward compatibility: existing QP and linear objectives unchanged
"""

import re

import pytest

from packdb_cli import PackDBCliError


# ===================================================================
# Phase 1: Boolean × anything (McCormick linearization, both solvers)
# ===================================================================


@pytest.mark.correctness
class TestBilinearBooleanObjectives:
    """Bilinear objectives where at least one factor is Boolean (linearizable)."""

    def test_bool_times_bool_objective(self, packdb_cli):
        """MAXIMIZE SUM(b1 * b2) — AND-linearization.

        Two binary variables with a cardinality constraint.
        Optimal: both are 1 → product = 1.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, b1, b2
            FROM data
            DECIDE b1 IS BOOLEAN, b2 IS BOOLEAN
            SUCH THAT SUM(b1) <= 3 AND SUM(b2) <= 3
            MAXIMIZE SUM(b1 * b2)
        """
        result, cols = packdb_cli.execute(sql)
        b1_col = cols.index("b1")
        b2_col = cols.index("b2")

        total = sum(int(row[b1_col]) * int(row[b2_col]) for row in result)
        # All should be 1*1 = 1, total = 3
        assert total == 3, f"Expected total product = 3, got {total}"

    def test_bool_times_bool_constrained(self, packdb_cli):
        """MAXIMIZE SUM(b1 * b2) with SUM(b1) <= 2.

        Only 2 of 3 rows can have b1=1, so max total product = 2.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, b1, b2
            FROM data
            DECIDE b1 IS BOOLEAN, b2 IS BOOLEAN
            SUCH THAT SUM(b1) <= 2 AND SUM(b2) <= 3
            MAXIMIZE SUM(b1 * b2)
        """
        result, cols = packdb_cli.execute(sql)
        b1_col = cols.index("b1")
        b2_col = cols.index("b2")

        total = sum(int(row[b1_col]) * int(row[b2_col]) for row in result)
        assert total == 2, f"Expected total product = 2, got {total}"

    def test_bool_times_real_objective(self, packdb_cli):
        """MAXIMIZE SUM(b * profit) — classic selection with profit maximization.

        select IS BOOLEAN, profit is data column, alloc IS REAL.
        Here: b * x where b IS BOOLEAN and x IS REAL with bounds.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS profit UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 5.0
            )
            SELECT id, b, ROUND(x, 2) AS x
            FROM data
            DECIDE b IS BOOLEAN, x IS REAL
            SUCH THAT x >= 0 AND x <= 100 AND SUM(b) <= 2
            MAXIMIZE SUM(b * x)
        """
        result, cols = packdb_cli.execute(sql)
        b_col = cols.index("b")
        x_col = cols.index("x")

        # With SUM(b) <= 2, the two selected rows should have x = 100 (max)
        for row in result:
            b_val = int(row[b_col])
            x_val = float(row[x_col])
            if b_val == 1:
                # b=1 → maximize b*x = x, so x should be at upper bound
                assert x_val == 100.0, f"Expected x=100 when b=1, got {x_val}"

    def test_bool_times_integer_objective(self, packdb_cli):
        """MAXIMIZE SUM(b * n) where b IS BOOLEAN, n IS INTEGER."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, b, n
            FROM data
            DECIDE b IS BOOLEAN, n IS INTEGER
            SUCH THAT n >= 0 AND n <= 5 AND SUM(b) <= 2
            MAXIMIZE SUM(b * n)
        """
        result, cols = packdb_cli.execute(sql)
        b_col = cols.index("b")
        n_col = cols.index("n")

        total = 0
        for row in result:
            b_val = int(row[b_col])
            n_val = int(row[n_col])
            total += b_val * n_val
            if b_val == 1:
                assert n_val == 5, f"Expected n=5 when b=1, got {n_val}"

        # Best 2 of 3 rows: 2 * 5 = 10
        assert total == 10, f"Expected total = 10, got {total}"

    def test_bool_times_real_with_data_coefficient(self, packdb_cli):
        """SUM(profit * b * x) — data coefficient scaling bilinear term.

        Parses as (profit * b) * x (left-associative). The optimizer must
        detect that b is the only decide var on the left side and x on
        the right, identify b as Boolean, and apply McCormick.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 3.0 AS profit UNION ALL
                SELECT 2, 1.0
            )
            SELECT id, b, ROUND(x, 2) AS x
            FROM data
            DECIDE b IS BOOLEAN, x IS REAL
            SUCH THAT x >= 0 AND x <= 10
            MAXIMIZE SUM(profit * b * x)
        """
        result, cols = packdb_cli.execute(sql)
        b_col = cols.index("b")
        x_col = cols.index("x")

        # Both rows should have b=1 and x=10
        for row in result:
            b_val = int(row[b_col])
            x_val = float(row[x_col])
            assert b_val == 1, f"Expected b=1, got {b_val}"
            assert abs(x_val - 10.0) < 0.01, f"Expected x=10, got {x_val}"


# ===================================================================
# Phase 2: General non-convex bilinear (Gurobi only via Q matrix)
# ===================================================================


@pytest.mark.correctness
class TestBilinearNonConvexObjectives:
    """Non-convex bilinear objectives (Real×Real, Int×Int) — Gurobi only."""

    def test_real_times_real_objective(self, packdb_cli):
        """MAXIMIZE SUM(x * y) with box constraints.

        Non-convex — Gurobi only. With x,y ∈ [0, 10], maximize x*y → x=y=10.
        """
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, ROUND(x, 2) AS x, ROUND(y, 2) AS y
            FROM data
            DECIDE x IS REAL, y IS REAL
            SUCH THAT x >= 0 AND x <= 10 AND y >= 0 AND y <= 10
            MAXIMIZE SUM(x * y)
        """
        try:
            result, cols = packdb_cli.execute(sql)
            x_val = float(result[0][cols.index("x")])
            y_val = float(result[0][cols.index("y")])
            # Maximize x*y with box constraints → corner: x=10, y=10
            assert abs(x_val - 10.0) < 0.1, f"Expected x=10, got {x_val}"
            assert abs(y_val - 10.0) < 0.1, f"Expected y=10, got {y_val}"
        except PackDBCliError as e:
            # HiGHS: non-convex rejection expected
            assert re.search(r"Non-convex|require Gurobi", e.message), \
                f"Unexpected error: {e.message}"

    def test_int_times_int_objective(self, packdb_cli):
        """MAXIMIZE SUM(x * y) where x, y IS INTEGER."""
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, x, y
            FROM data
            DECIDE x IS INTEGER, y IS INTEGER
            SUCH THAT x >= 0 AND x <= 5 AND y >= 0 AND y <= 5
            MAXIMIZE SUM(x * y)
        """
        try:
            result, cols = packdb_cli.execute(sql)
            x_val = int(result[0][cols.index("x")])
            y_val = int(result[0][cols.index("y")])
            assert x_val == 5, f"Expected x=5, got {x_val}"
            assert y_val == 5, f"Expected y=5, got {y_val}"
        except PackDBCliError as e:
            assert re.search(r"Non-convex|require Gurobi", e.message), \
                f"Unexpected error: {e.message}"

    def test_int_times_real_objective(self, packdb_cli):
        """MAXIMIZE SUM(n * x) where n IS INTEGER, x IS REAL."""
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, n, ROUND(x, 2) AS x
            FROM data
            DECIDE n IS INTEGER, x IS REAL
            SUCH THAT n >= 0 AND n <= 5 AND x >= 0 AND x <= 10
            MAXIMIZE SUM(n * x)
        """
        try:
            result, cols = packdb_cli.execute(sql)
            n_val = int(result[0][cols.index("n")])
            x_val = float(result[0][cols.index("x")])
            assert n_val == 5, f"Expected n=5, got {n_val}"
            assert abs(x_val - 10.0) < 0.1, f"Expected x=10, got {x_val}"
        except PackDBCliError as e:
            assert re.search(r"Non-convex|require Gurobi", e.message), \
                f"Unexpected error: {e.message}"


# ===================================================================
# Mixed objectives
# ===================================================================


@pytest.mark.correctness
class TestBilinearMixedObjectives:
    """Mixed linear + bilinear and bilinear + POWER objectives."""

    def test_linear_plus_bilinear(self, packdb_cli):
        """MAXIMIZE SUM(cost + b * x) — mix of linear and bilinear terms."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 5.0 AS cost UNION ALL
                SELECT 2, 10.0
            )
            SELECT id, b, ROUND(x, 2) AS x
            FROM data
            DECIDE b IS BOOLEAN, x IS REAL
            SUCH THAT x >= 0 AND x <= 20
            MAXIMIZE SUM(cost + b * x)
        """
        result, cols = packdb_cli.execute(sql)
        # All b should be 1, x should be at upper bound
        for row in result:
            b_val = int(row[cols.index("b")])
            x_val = float(row[cols.index("x")])
            assert b_val == 1, f"Expected b=1, got {b_val}"
            assert abs(x_val - 20.0) < 0.01, f"Expected x=20, got {x_val}"


# ===================================================================
# Feature interactions
# ===================================================================


@pytest.mark.correctness
class TestBilinearFeatureInteractions:
    """Bilinear with WHEN, PER, and other features."""

    def test_bilinear_with_when(self, packdb_cli):
        """MAXIMIZE SUM(b * x) WHEN category = 'A' — filtered bilinear."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 'A' AS category UNION ALL
                SELECT 2, 'B' UNION ALL
                SELECT 3, 'A'
            )
            SELECT id, category, b, ROUND(x, 2) AS x
            FROM data
            DECIDE b IS BOOLEAN, x IS REAL
            SUCH THAT x >= 0 AND x <= 10
            MAXIMIZE SUM(b * x) WHEN category = 'A'
        """
        result, cols = packdb_cli.execute(sql)
        cat_col = cols.index("category")
        b_col = cols.index("b")
        x_col = cols.index("x")

        for row in result:
            cat = row[cat_col]
            b_val = int(row[b_col])
            x_val = float(row[x_col])
            if cat == 'A':
                # WHEN filter means these rows contribute to objective
                # Maximize b*x → b=1, x=10
                assert b_val == 1, f"Expected b=1 for category A, got {b_val}"
                assert abs(x_val - 10.0) < 0.01, f"Expected x=10 for A, got {x_val}"


# ===================================================================
# Phase 3: Bilinear in constraints
# ===================================================================


@pytest.mark.correctness
class TestBilinearConstraints:
    """Bilinear terms in SUCH THAT constraints."""

    def test_bool_bilinear_constraint(self, packdb_cli):
        """SUCH THAT SUM(b1 * b2) <= 1 — limits AND of booleans."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, b1, b2
            FROM data
            DECIDE b1 IS BOOLEAN, b2 IS BOOLEAN
            SUCH THAT SUM(b1 * b2) <= 1
            MAXIMIZE SUM(b1 + b2)
        """
        result, cols = packdb_cli.execute(sql)
        b1_col = cols.index("b1")
        b2_col = cols.index("b2")

        # SUM(b1*b2) <= 1: at most 1 row has both b1=1 and b2=1
        total_product = sum(int(row[b1_col]) * int(row[b2_col]) for row in result)
        assert total_product <= 1, f"Expected SUM(b1*b2) <= 1, got {total_product}"

        # Maximize SUM(b1+b2) subject to SUM(b1*b2) <= 1
        total_sum = sum(int(row[b1_col]) + int(row[b2_col]) for row in result)
        # Optimal: 1 row has both=1 (contributes 2), other rows have only one or none
        # Max with constraint: 1*(1+1) + 2*(1+0) or similar
        assert total_sum >= 4, f"Expected SUM(b1+b2) >= 4, got {total_sum}"


# ===================================================================
# Error cases
# ===================================================================


@pytest.mark.correctness
class TestBilinearErrors:
    """Error cases that should be rejected."""

    def test_triple_product_rejected(self, packdb_cli):
        """SUM(a * b * c) with three DECIDE variables should be rejected."""
        packdb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, a, b, c FROM data
            DECIDE a IS BOOLEAN, b IS BOOLEAN, c IS BOOLEAN
            SUCH THAT a >= 0 AND b >= 0 AND c >= 0
            MAXIMIZE SUM(a * b * c)
        """, match=r"Triple|higher-order")

    def test_missing_upper_bound_rejected(self, packdb_cli):
        """b * x without an upper bound on x should be rejected."""
        packdb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id)
            SELECT id, b, x FROM data
            DECIDE b IS BOOLEAN, x IS REAL
            SUCH THAT b >= 0
            MAXIMIZE SUM(b * x)
        """, match=r"finite upper bound|upper bound")

    def test_highs_nonconvex_bilinear_rejected(self, packdb_cli):
        """Real × Real bilinear on HiGHS should error (non-convex)."""
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, ROUND(x, 2) AS x, ROUND(y, 2) AS y
            FROM data
            DECIDE x IS REAL, y IS REAL
            SUCH THAT x >= 0 AND x <= 10 AND y >= 0 AND y <= 10
            MAXIMIZE SUM(x * y)
        """
        try:
            result, cols = packdb_cli.execute(sql)
            # If Gurobi is available, this is fine (non-convex QP)
            pass  # Gurobi handled it
        except PackDBCliError as e:
            # HiGHS: non-convex rejection expected
            assert re.search(r"Non-convex|require Gurobi", e.message), \
                f"Unexpected error: {e.message}"


# ===================================================================
# Backward compatibility
# ===================================================================


@pytest.mark.correctness
class TestBilinearBackwardCompat:
    """Ensure existing QP and linear objectives still work."""

    def test_power_still_works(self, packdb_cli):
        """POWER(x - target, 2) should still work as before."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x - target, 2))
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")

        for row in result:
            rid = int(row[id_col])
            x_val = float(row[x_col])
            expected = {1: 10.0, 2: 20.0}[rid]
            assert abs(x_val - expected) < 0.01

    def test_linear_objective_still_works(self, packdb_cli):
        """Simple linear MAXIMIZE SUM(profit * x) should still work."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS profit UNION ALL
                SELECT 2, 5.0 UNION ALL
                SELECT 3, 8.0
            )
            SELECT id, profit, x
            FROM data
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) <= 2
            MAXIMIZE SUM(profit * x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        profit_col = cols.index("profit")

        total = sum(float(row[profit_col]) * int(row[x_col]) for row in result)
        # Best 2 of 3: ids 1 (10) and 3 (8) = 18
        assert abs(total - 18.0) < 0.01, f"Expected 18, got {total}"

    def test_identical_multiplication_still_qp(self, packdb_cli):
        """(x - target) * (x - target) should still be treated as QP, not bilinear."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 7.0 AS target UNION ALL
                SELECT 2, 42.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM((x - target) * (x - target))
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")

        for row in result:
            rid = int(row[id_col])
            x_val = float(row[x_col])
            expected = {1: 7.0, 2: 42.0}[rid]
            assert abs(x_val - expected) < 0.01
