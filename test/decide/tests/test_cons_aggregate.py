"""Tests for SUM-based aggregate constraints.

Covers:
  - q08_marketing_campaign: SUM(x * constant) <= budget
  - order_selection: SUM(x) <= count_limit
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


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

    cmp = compare_solutions(
        packdb_result, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("c_acctbal")])},
    )

    perf_tracker.record(
        "q08_marketing_campaign", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
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

    cmp = compare_solutions(
        packdb_result, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("o_totalprice")])},
    )

    perf_tracker.record(
        "order_selection", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_gte_with_maximize(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Set-covering pattern: SUM(x) >= 10 with MAXIMIZE objective."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 10
            AND SUM(x * l_quantity) <= 500
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
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_gte_maximize")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        ">=", 10.0, name="min_count",
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 500.0, name="capacity",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        packdb_result, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_extendedprice")])},
    )

    perf_tracker.record(
        "sum_gte_maximize", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
