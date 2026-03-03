"""Tests for the PER keyword on aggregate constraints.

Covers:
  - Basic PER: one constraint per distinct group value
  - WHEN + PER composition: filter first, then group
  - PER on objective (no-op, treated as global SUM)
  - Multiple constraints with PER
  - PER with constant coefficient
  - Error: PER on per-row (non-aggregate) constraint
  - Error: PER column references a DECIDE variable
"""

import time

import pytest

import packdb
from solver.types import VarType, ObjSense
from comparison.compare import assert_optimal_match


# ---------------------------------------------------------------------------
# Correctness tests
# ---------------------------------------------------------------------------


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_basic(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Basic PER: SUM(x) <= 5 PER s_nationkey — at most 5 suppliers per nation."""
    sql = """
        SELECT s_suppkey, s_acctbal, s_nationkey, x FROM supplier
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE),
               CAST(s_nationkey AS BIGINT)
        FROM supplier
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_basic")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # One constraint per distinct s_nationkey value
    groups: dict[int, list[int]] = {}
    for i, row in enumerate(data):
        nk = row[2]
        groups.setdefault(nk, []).append(i)

    for nk, members in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in members},
            "<=", 5.0, name=f"per_nation_{nk}",
        )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("s_acctbal")])},
    )

    perf_tracker.record(
        "per_basic", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames),
        len(groups), result.objective_value, oracle_solver.solver_name(),
    )


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_with_when(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN + PER composition: filter by returnflag='R', then group by l_suppkey."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, l_suppkey, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 20 WHEN l_returnflag = 'R' PER l_suppkey
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag,
               CAST(l_suppkey AS BIGINT)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_with_when")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # WHEN filters first (returnflag='R'), then PER groups by l_suppkey
    groups: dict[int, list[int]] = {}
    for i, row in enumerate(data):
        if row[4] == 'R':  # WHEN filter
            sk = row[5]
            groups.setdefault(sk, []).append(i)

    for sk, members in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: data[i][3] for i in members},
            "<=", 20.0, name=f"per_supp_{sk}",
        )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "per_with_when", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames),
        len(groups), result.objective_value, oracle_solver.solver_name(),
    )


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_on_objective_noop(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """PER on objective is a no-op — should produce same result as global SUM."""
    sql = """
        SELECT s_suppkey, s_acctbal, s_nationkey, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 30
        MAXIMIZE SUM(x * s_acctbal) PER s_nationkey
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE),
               CAST(s_nationkey AS BIGINT)
        FROM supplier WHERE s_nationkey < 5
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_obj_noop")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Global constraint (no PER)
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 30.0, name="total_limit",
    )
    # PER on objective is no-op: same as global SUM(x * s_acctbal)
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("s_acctbal")])},
    )

    perf_tracker.record(
        "per_obj_noop", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
    )


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_with_global_constraint(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """PER constraint + unconditional global constraint together."""
    sql = """
        SELECT s_suppkey, s_acctbal, s_nationkey, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5 PER s_nationkey
            AND SUM(x) <= 15
        MAXIMIZE SUM(x * s_acctbal)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE),
               CAST(s_nationkey AS BIGINT)
        FROM supplier WHERE s_nationkey < 5
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_with_global")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # PER constraint: at most 5 per nation
    groups: dict[int, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[2], []).append(i)

    for nk, members in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in members},
            "<=", 5.0, name=f"per_nation_{nk}",
        )
    # Global constraint: at most 15 total
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 15.0, name="total_limit",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("s_acctbal")])},
    )

    perf_tracker.record(
        "per_with_global", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames),
        len(groups) + 1, result.objective_value, oracle_solver.solver_name(),
    )


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_weighted_coefficient(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """PER with weighted SUM: SUM(x * s_acctbal) <= 20000 PER s_nationkey."""
    sql = """
        SELECT s_suppkey, s_acctbal, s_nationkey, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * s_acctbal) <= 20000 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE),
               CAST(s_nationkey AS BIGINT)
        FROM supplier WHERE s_nationkey < 5
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_weighted")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # One weighted constraint per nation
    groups: dict[int, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[2], []).append(i)

    for nk, members in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: data[i][1] for i in members},
            "<=", 20000.0, name=f"budget_nation_{nk}",
        )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("s_acctbal")])},
    )

    perf_tracker.record(
        "per_weighted", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames),
        len(groups), result.objective_value, oracle_solver.solver_name(),
    )


@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_per_minimize(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """PER with MINIMIZE: pick at least 2 per nation, minimize total acctbal."""
    sql = """
        SELECT s_suppkey, s_acctbal, s_nationkey, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 2 PER s_nationkey
        MINIMIZE SUM(x * s_acctbal)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE),
               CAST(s_nationkey AS BIGINT)
        FROM supplier WHERE s_nationkey < 5
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("per_minimize")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # At least 2 per nation
    groups: dict[int, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[2], []).append(i)

    for nk, members in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in members},
            ">=", 2.0, name=f"min_nation_{nk}",
        )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("s_acctbal")])},
    )

    perf_tracker.record(
        "per_minimize", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames),
        len(groups), result.objective_value, oracle_solver.solver_name(),
    )


# ---------------------------------------------------------------------------
# Error tests
# ---------------------------------------------------------------------------


@pytest.mark.per_clause
@pytest.mark.error_binder
@pytest.mark.error
class TestPerBinderErrors:
    """PER binder should reject invalid PER usage."""

    def test_per_on_non_aggregate(self, packdb_conn):
        """PER requires an aggregate (SUM) constraint — per-row is invalid."""
        with pytest.raises(packdb.BinderException, match=r"PER.*aggregate"):
            packdb_conn.execute("""
                SELECT s_suppkey, s_acctbal, s_nationkey, x FROM supplier
                DECIDE x IS BOOLEAN
                SUCH THAT x <= 1 PER s_nationkey
                MAXIMIZE SUM(x * s_acctbal)
            """)

    def test_per_decide_variable_column(self, packdb_conn):
        """PER column must be a table column, not a DECIDE variable."""
        with pytest.raises(packdb.BinderException, match=r"PER.*DECIDE"):
            packdb_conn.execute("""
                SELECT s_suppkey, s_acctbal, x FROM supplier
                DECIDE x IS BOOLEAN
                SUCH THAT SUM(x) <= 5 PER x
                MAXIMIZE SUM(x * s_acctbal)
            """)
