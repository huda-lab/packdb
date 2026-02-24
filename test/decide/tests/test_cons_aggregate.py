"""Tests for SUM-based aggregate constraints.

Covers:
  - q08_marketing_campaign: SUM(x * constant) <= budget
  - order_selection: SUM(x) <= count_limit
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import assert_optimal_match


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q08_marketing_campaign(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Marketing: select customers, cost=10/customer, budget=500."""
    sql = """
        SELECT c_custkey, c_acctbal, x
        FROM customer
        WHERE c_nationkey = 1
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * 10) <= 500
        MAXIMIZE SUM(x * c_acctbal)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(c_custkey AS BIGINT),
               CAST(c_acctbal AS DOUBLE)
        FROM customer WHERE c_nationkey = 1
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("q08_marketing")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Constraint: SUM(x_i * 10) <= 500  => each coefficient is 10
    oracle_solver.add_constraint(
        {vnames[i]: 10.0 for i in range(len(data))},
        "<=", 500.0, name="budget",
    )
    # Objective: MAXIMIZE SUM(x_i * c_acctbal_i)
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("c_acctbal")])},
    )

    perf_tracker.record(
        "q08_marketing_campaign", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
    )


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_order_selection(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Select orders: SUM(x) <= 300, maximize total price."""
    sql = """
        SELECT x, o_orderkey, o_totalprice
        FROM orders
        WHERE o_orderdate >= '1995-01-01' AND o_orderdate < '1995-02-01'
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 300
        MAXIMIZE SUM(x * o_totalprice)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(o_orderkey AS BIGINT),
               CAST(o_totalprice AS DOUBLE)
        FROM orders
        WHERE o_orderdate >= '1995-01-01' AND o_orderdate < '1995-02-01'
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("order_selection")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # SUM(x) <= 300  => each coefficient is 1.0
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 300.0, name="count_limit",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("o_totalprice")])},
    )

    perf_tracker.record(
        "order_selection", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
    )
