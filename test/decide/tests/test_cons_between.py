"""Tests for BETWEEN constraints.

Covers:
  - q10_logic_dependency: x BETWEEN 0 AND 5, with aggregate constraint
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import assert_optimal_match


@pytest.mark.var_integer
@pytest.mark.cons_between
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q10_logic_dependency(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """BETWEEN constraint: x BETWEEN 0 AND 5, maximize count."""
    sql = """
        SELECT l_orderkey, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey < 20
        DECIDE x IS INTEGER
        SUCH THAT x BETWEEN 0 AND 5
          AND SUM(x * l_extendedprice) <= 10000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("q10_logic")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        # BETWEEN 0 AND 5 => lb=0, ub=5
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=5.0)

    # SUM(x * l_extendedprice) <= 10000
    oracle_solver.add_constraint(
        {vnames[i]: data[i][1] for i in range(len(data))},
        "<=", 10000.0, name="price_limit",
    )
    # MAXIMIZE SUM(x)
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "q10_logic_dependency", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
    )
