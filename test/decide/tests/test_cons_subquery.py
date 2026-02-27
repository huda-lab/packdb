"""Tests for subquery on constraint RHS.

Covers:
  - q04_subquery_rhs: SUM(x * price) <= (SELECT AVG(c_acctbal) ...)
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_boolean
@pytest.mark.cons_subquery
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.sql_subquery
@pytest.mark.correctness
def test_q04_subquery_rhs(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Subquery RHS: total price <= avg customer acctbal for nation 10."""
    # Note: subquery table must be fully qualified — DECIDE subqueries
    # are evaluated in a separate context that doesn't inherit search_path.
    sql = """
        SELECT o_orderkey, o_totalprice, x
        FROM orders
        WHERE o_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * o_totalprice) <= (SELECT AVG(c_acctbal) FROM tpch.customer WHERE c_nationkey = 10)
        MAXIMIZE SUM(x * o_totalprice)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    # Fetch data + resolve subquery via duckdb
    data = duckdb_conn.execute("""
        SELECT CAST(o_orderkey AS BIGINT),
               CAST(o_totalprice AS DOUBLE)
        FROM orders WHERE o_orderkey < 100
    """).fetchall()

    rhs_val = duckdb_conn.execute("""
        SELECT CAST(AVG(c_acctbal) AS DOUBLE) FROM tpch.customer WHERE c_nationkey = 10
    """).fetchone()[0]

    t_build = time.perf_counter()
    oracle_solver.create_model("q04_subquery")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][1] for i in range(len(data))},
        "<=", float(rhs_val), name="budget",
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
        "q04_subquery_rhs", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
