"""Tests for quadratic programming (QP) objectives.

Covers:
  - POWER(linear_expr, 2) syntax
  - ** operator form
  - (expr)*(expr) multiplication form
  - MINIMIZE convex QP (standard)
  - MAXIMIZE concave QP (negated quadratic, Case A)
  - MAXIMIZE convex QP (non-convex, Case B — Gurobi only)
  - Mixed linear + quadratic (linear constraints with quadratic objective)
  - REAL variable QP (continuous)
  - Error rejection: POWER with exponent != 2, product of different vars
"""

import re
import time

import pytest

from packdb_cli import PackDBCliError


# ===================================================================
# Basic QP correctness tests (using inline tables via packdb_cli)
# ===================================================================


@pytest.mark.obj_minimize
@pytest.mark.quadratic
@pytest.mark.correctness
class TestQuadraticBasic:
    """Basic QP smoke tests using small inline data."""

    def test_power_syntax_unconstrained(self, packdb_cli):
        """MINIMIZE SUM(POWER(x - target, 2)) with box constraints.

        Optimal: x_i = target_i (closest point in [0, 100] to target).
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
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
            expected = {1: 10.0, 2: 20.0, 3: 30.0}[rid]
            assert abs(x_val - expected) < 0.01, \
                f"Row {rid}: expected x={expected}, got x={x_val}"

    def test_starstar_operator(self, packdb_cli):
        """** operator should behave identically to POWER."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 15.0 AS target UNION ALL
                SELECT 2, 25.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM((x - target) ** 2)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")

        for row in result:
            rid = int(row[id_col])
            x_val = float(row[x_col])
            expected = {1: 15.0, 2: 25.0}[rid]
            assert abs(x_val - expected) < 0.01

    def test_multiplication_form(self, packdb_cli):
        """(expr)*(expr) form should behave identically to POWER."""
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

    def test_qp_with_binding_constraint(self, packdb_cli):
        """QP where box constraints clip the solution.

        target = [5, 50], bounds = [10, 40]
        Expected: x = [10, 40] (clipped to bounds).
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 5.0 AS target UNION ALL
                SELECT 2, 50.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 10 AND x <= 40
            MINIMIZE SUM(POWER(x - target, 2))
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")

        for row in result:
            rid = int(row[id_col])
            x_val = float(row[x_col])
            expected = {1: 10.0, 2: 40.0}[rid]
            assert abs(x_val - expected) < 0.01

    def test_qp_with_aggregate_constraint(self, packdb_cli):
        """QP with an aggregate constraint: SUM(x) = 60.

        targets = [10, 20, 30]. Unconstrained optimum sums to 60,
        so the constraint is exactly satisfied at the unconstrained optimum.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT ROUND(SUM(x), 2) AS total_x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100 AND SUM(x) = 60
            MINIMIZE SUM(POWER(x - target, 2))
        """
        result, cols = packdb_cli.execute(sql)
        total = float(result[0][cols.index("total_x")])
        assert abs(total - 60.0) < 0.1

    def test_qp_simple_squared_variable(self, packdb_cli):
        """MINIMIZE SUM(POWER(x, 2)) — simplest QP: x should be 0 everywhere."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x, 2))
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        for row in result:
            x_val = float(row[x_col])
            assert abs(x_val) < 0.01, f"Expected x=0, got x={x_val}"

    def test_qp_with_when(self, packdb_cli):
        """QP with WHEN filter — only matching rows contribute to objective."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target, 'A' AS grp UNION ALL
                SELECT 2, 20.0, 'B' UNION ALL
                SELECT 3, 30.0, 'A' UNION ALL
                SELECT 4, 40.0, 'B'
            )
            SELECT id, target, grp, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x - target, 2)) WHEN grp = 'A'
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")
        grp_col = cols.index("grp")
        target_col = cols.index("target")

        # Rows matching WHEN (grp='A') should track their target
        for row in result:
            grp = str(row[grp_col])
            x_val = float(row[x_col])
            target = float(row[target_col])
            if grp == 'A':
                assert abs(x_val - target) < 0.01, \
                    f"Row {row[id_col]} (grp=A): expected x≈{target}, got x={x_val}"

    def test_qp_multiple_variables(self, packdb_cli):
        """QP with two REAL decision variables — both in constraints, one in QP objective.

        Tests that multiple variables coexist correctly when the QP objective
        references only one variable. x should track targets; y is constrained
        independently via a per-row upper bound.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target, 50.0 AS cap UNION ALL
                SELECT 2, 20.0, 50.0 UNION ALL
                SELECT 3, 30.0, 50.0
            )
            SELECT id, ROUND(x, 4) AS x, ROUND(y, 4) AS y
            FROM data
            DECIDE x IS REAL, y IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND y >= 0 AND y <= cap
            MINIMIZE SUM(POWER(x - target, 2))
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")

        expected_x = {1: 10.0, 2: 20.0, 3: 30.0}
        for row in result:
            rid = int(row[id_col])
            x_val = float(row[x_col])
            assert abs(x_val - expected_x[rid]) < 0.01, \
                f"Row {rid}: expected x={expected_x[rid]}, got x={x_val}"


# ===================================================================
# Error tests
# ===================================================================


# ===================================================================
# MAXIMIZE quadratic tests (Case A: concave, Case B: convex non-convex)
# ===================================================================


@pytest.mark.quadratic
@pytest.mark.obj_maximize
@pytest.mark.correctness
class TestQuadraticMaximize:
    """Tests for MAXIMIZE with quadratic objectives."""

    def test_maximize_negated_power_case_a(self, packdb_cli):
        """Case A: MAXIMIZE SUM(-POWER(x - target, 2)) — concave quadratic.

        Equivalent to MINIMIZE SUM(POWER(x - target, 2)).
        Optimal: x_i = target_i (minimizes squared deviation = maximizes negative).
        Both Gurobi and HiGHS support this (convex after internal sense+Hessian flip).
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
            MAXIMIZE SUM(-POWER(x - target, 2))
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")

        for row in result:
            rid = int(row[id_col])
            x_val = float(row[x_col])
            expected = {1: 10.0, 2: 20.0, 3: 30.0}[rid]
            assert abs(x_val - expected) < 0.01, \
                f"Row {rid}: expected x={expected}, got x={x_val}"

    def test_maximize_negated_power_with_binding_constraint(self, packdb_cli):
        """Case A with binding constraints: targets outside bounds get clipped.

        target = [5, 50], bounds = [10, 40]
        Expected: x = [10, 40] (same as MINIMIZE SUM(POWER(x - target, 2))).
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 5.0 AS target UNION ALL
                SELECT 2, 50.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 10 AND x <= 40
            MAXIMIZE SUM(-POWER(x - target, 2))
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")

        for row in result:
            rid = int(row[id_col])
            x_val = float(row[x_col])
            expected = {1: 10.0, 2: 40.0}[rid]
            assert abs(x_val - expected) < 0.01, \
                f"Row {rid}: expected x={expected}, got x={x_val}"

    def test_maximize_negated_starstar_syntax(self, packdb_cli):
        """Case A with ** syntax: MAXIMIZE SUM(-(x - target) ** 2)."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 15.0 AS target UNION ALL
                SELECT 2, 25.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
            MAXIMIZE SUM(-((x - target) ** 2))
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")

        for row in result:
            rid = int(row[id_col])
            x_val = float(row[x_col])
            expected = {1: 15.0, 2: 25.0}[rid]
            assert abs(x_val - expected) < 0.01

    def test_maximize_convex_power_case_b(self, packdb_cli):
        """Case B: MAXIMIZE SUM(POWER(x, 2)) — non-convex (Gurobi only).

        With bounds [0, 10], the maximum of x² is at x=10 (boundary).
        Succeeds on Gurobi (NonConvex=2), errors on HiGHS.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 10
            MAXIMIZE SUM(POWER(x, 2))
        """
        try:
            result, cols = packdb_cli.execute(sql)
            # Gurobi path: should push x to boundary (x=10 maximizes x²)
            x_col = cols.index("x")
            for row in result:
                x_val = float(row[x_col])
                assert abs(x_val - 10.0) < 0.01, \
                    f"Expected x=10 (boundary), got x={x_val}"
        except PackDBCliError as e:
            # HiGHS path: non-convex quadratic rejected
            assert re.search(r"Non-convex quadratic objectives require Gurobi", e.message), \
                f"Unexpected error: {e.message}"

    def test_maximize_convex_power_integer_case_b(self, packdb_cli):
        """Case B with INTEGER variables: MAXIMIZE SUM(POWER(x - 5, 2)).

        With bounds [0, 10], maximum distance from 5 is at x=0 or x=10.
        Both give (x-5)² = 25.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2
            )
            SELECT id, x
            FROM data
            DECIDE x IS INTEGER
            SUCH THAT x >= 0 AND x <= 10
            MAXIMIZE SUM(POWER(x - 5, 2))
        """
        try:
            result, cols = packdb_cli.execute(sql)
            # Gurobi path: x should be 0 or 10 (both maximize distance from 5)
            x_col = cols.index("x")
            for row in result:
                x_val = int(row[x_col])
                assert x_val in (0, 10), \
                    f"Expected x=0 or x=10, got x={x_val}"
        except PackDBCliError as e:
            assert re.search(r"Non-convex quadratic objectives require Gurobi", e.message), \
                f"Unexpected error: {e.message}"

    def test_maximize_negated_power_with_sum_constraint(self, packdb_cli):
        """Case A with aggregate constraint: MAXIMIZE SUM(-POWER(x - target, 2)) + SUM(x) = K.

        targets = [10, 20, 30], SUM(x) = 60 (sum equals unconstrained optimum).
        Constraint is not binding — solution should match targets exactly.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT ROUND(SUM(x), 2) AS total_x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100 AND SUM(x) = 60
            MAXIMIZE SUM(-POWER(x - target, 2))
        """
        result, cols = packdb_cli.execute(sql)
        total = float(result[0][cols.index("total_x")])
        assert abs(total - 60.0) < 0.1


# ===================================================================
# Error tests
# ===================================================================


@pytest.mark.error
@pytest.mark.quadratic
class TestQuadraticErrors:
    """Queries that should be rejected by the binder or physical operator."""

    def test_power_exponent_3_rejected(self, packdb_cli):
        """POWER(expr, 3) is not supported — expanded form is non-linear."""
        packdb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id, 10.0 AS target)
            SELECT id, x FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x - target, 3))
        """, match=r"Product of different DECIDE variable expressions|must remain linear")

    def test_product_of_different_vars_rejected(self, packdb_cli):
        """x * y where both are DECIDE variables should be rejected."""
        packdb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id, 10.0 AS val)
            SELECT id, x, y FROM data
            DECIDE x IS REAL, y IS REAL
            SUCH THAT x >= 0 AND x <= 10 AND y >= 0 AND y <= 10
            MINIMIZE SUM(x * y)
        """, match=r"Product of different DECIDE variable expressions")

    def test_power_with_variable_exponent_rejected(self, packdb_cli):
        """POWER(expr, non_constant) should be rejected."""
        packdb_cli.assert_error("""
            WITH data AS (SELECT 1 AS id, 10.0 AS target, 2 AS exp)
            SELECT id, x FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
            MINIMIZE SUM(POWER(x - target, exp))
        """, match=r"Non-integer exponents are not supported")


# ===================================================================
# TPC-H based QP tests (with oracle comparison)
# ===================================================================


@pytest.mark.quadratic
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_minimize_squared_deviation_tpch(packdb_cli, duckdb_conn, perf_tracker):
    """QP on TPC-H partsupp: minimize squared deviation from mean supply cost.

    Each row gets a REAL decision variable x that is constrained to [0, 1000].
    Objective: MINIMIZE SUM(POWER(x - ps_supplycost, 2))
    Without aggregate constraints, optimal x_i = ps_supplycost_i.
    We verify the objective value is near 0.
    """
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost, ROUND(x, 2) AS x
        FROM partsupp
        WHERE ps_partkey <= 10
        DECIDE x IS REAL
        SUCH THAT x >= 0 AND x <= 1000
        MINIMIZE SUM(POWER(x - ps_supplycost, 2))
    """
    t0 = time.perf_counter()
    result, cols = packdb_cli.execute(sql)
    elapsed = time.perf_counter() - t0

    x_col = cols.index("x")
    cost_col = cols.index("ps_supplycost")

    total_sq_dev = 0.0
    for row in result:
        x_val = float(row[x_col])
        cost = float(row[cost_col])
        total_sq_dev += (x_val - cost) ** 2

    assert total_sq_dev < 1.0, \
        f"Total squared deviation should be near 0, got {total_sq_dev}"

    perf_tracker.record(
        "qp_squared_deviation", elapsed, 0, 0,
        len(result), len(result), 0, total_sq_dev, "packdb",
    )


@pytest.mark.quadratic
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_with_sum_constraint_tpch(packdb_cli, duckdb_conn, perf_tracker):
    """QP with SUM constraint on TPC-H data.

    MINIMIZE SUM(POWER(x - ps_supplycost, 2))
    SUCH THAT SUM(x) >= total_cost * 1.1 (inflate by 10%)

    The constraint forces x values above their targets on average,
    distributing the excess equally (QP property).
    """
    total_cost = duckdb_conn.execute("""
        SELECT SUM(CAST(ps_supplycost AS DOUBLE))
        FROM partsupp WHERE ps_partkey <= 10
    """).fetchone()[0]

    inflated = round(total_cost * 1.1, 2)

    sql = f"""
        SELECT ps_partkey, ps_suppkey, ps_supplycost, ROUND(x, 2) AS x
        FROM partsupp
        WHERE ps_partkey <= 10
        DECIDE x IS REAL
        SUCH THAT x >= 0 AND x <= 10000 AND SUM(x) >= {inflated}
        MINIMIZE SUM(POWER(x - ps_supplycost, 2))
    """
    t0 = time.perf_counter()
    result, cols = packdb_cli.execute(sql)
    elapsed = time.perf_counter() - t0

    x_col = cols.index("x")
    actual_sum = sum(float(row[x_col]) for row in result)

    assert actual_sum >= inflated - 0.1, \
        f"SUM(x) = {actual_sum} should be >= {inflated}"

    perf_tracker.record(
        "qp_sum_constraint", elapsed, 0, 0,
        len(result), len(result), 1, actual_sum, "packdb",
    )
