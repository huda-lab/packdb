"""Tests for per-row constraints (variable bounds per row).

Covers:
  - q07_row_wise_bounds: x <= 5 per row, plus global SUM constraint
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import assert_optimal_match


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.cons_mixed
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_q07_row_wise_bounds(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Row-wise bounds: x <= 5 for each row, plus SUM(x) <= 100."""
    sql = """
        SELECT ps_partkey, ps_availqty, x
        FROM partsupp
        WHERE ps_partkey < 20
        DECIDE x IS INTEGER
        SUCH THAT x <= 5
          AND SUM(x) <= 100
        MAXIMIZE SUM(x * ps_availqty)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_availqty AS DOUBLE)
        FROM partsupp WHERE ps_partkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("q07_row_wise")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        # x <= 5 is a per-row upper bound; lb=0 (default INTEGER)
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=5.0)

    # Global constraint: SUM(x) <= 100
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 100.0, name="total_limit",
    )
    # Objective: MAXIMIZE SUM(x * ps_availqty)
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("ps_availqty")])},
    )

    perf_tracker.record(
        "q07_row_wise_bounds", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
    )
