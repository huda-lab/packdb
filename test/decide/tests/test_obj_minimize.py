"""Tests for MINIMIZE objective.

Covers:
  - q09_minimize_cost: minimize supplier acctbal selection
  - min_cost_supplier: minimize supply cost to meet demand
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import assert_optimal_match


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_q09_minimize_cost(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Minimize cost: select >= 10 suppliers, minimize total acctbal."""
    # Uses nationkey <= 5 to ensure enough suppliers (20) for the >= 10 constraint.
    # The original query used nationkey = 5, but that only has 3 suppliers at SF-0.01.
    sql = """
        SELECT s_suppkey, s_acctbal, x
        FROM supplier
        WHERE s_nationkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 10
        MINIMIZE SUM(x * s_acctbal)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(s_suppkey AS BIGINT),
               CAST(s_acctbal AS DOUBLE)
        FROM supplier WHERE s_nationkey <= 5
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("q09_minimize")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # SUM(x) >= 10
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        ">=", 10.0, name="min_count",
    )
    # MINIMIZE SUM(x * s_acctbal)
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
        "q09_minimize_cost", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
    )


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_min_cost_supplier(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Minimize supply cost: need >= 1000 total availqty."""
    sql = """
        SELECT x, ps_partkey, ps_suppkey, ps_supplycost, ps_availqty
        FROM partsupp
        WHERE ps_partkey <= 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * ps_availqty) >= 1000
        MINIMIZE SUM(x * ps_supplycost)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE),
               CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey <= 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("min_cost_supplier")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # SUM(x * availqty) >= 1000
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        ">=", 1000.0, name="demand",
    )
    # MINIMIZE SUM(x * supplycost)
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("ps_supplycost")])},
    )

    perf_tracker.record(
        "min_cost_supplier", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
    )
