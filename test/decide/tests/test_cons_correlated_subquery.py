"""Tests for correlated subqueries in DECIDE constraints.

Covers:
  - Per-row constraint with correlated subquery: x <= (SELECT ... WHERE correlation)
  - Verifies decorrelation into JOIN and per-row RHS evaluation.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.cons_aggregate
@pytest.mark.cons_subquery
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_correlated_subquery_perrow_bound(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Correlated subquery provides per-row upper bound from another table.

    x <= (SELECT p_size FROM part WHERE p_partkey = ps_partkey) bounds each
    partsupp row's x by the corresponding part's size. Combined with a global
    budget constraint on supplycost.
    """
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost, x
        FROM partsupp
        WHERE ps_partkey < 20
        DECIDE x IS INTEGER
        SUCH THAT x <= (SELECT CAST(p_size AS INTEGER) FROM part WHERE p_partkey = ps_partkey)
          AND SUM(x * ps_supplycost) <= 5000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE),
               CAST(p_size AS DOUBLE)
        FROM partsupp
        JOIN part ON p_partkey = ps_partkey
        WHERE ps_partkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("correlated_subquery_perrow")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, vn in enumerate(vnames):
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=data[i][3])

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 5000.0, name="budget",
    )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        packdb_result, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "correlated_subquery_perrow", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.cons_subquery
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_correlated_subquery_boolean_filter(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Correlated subquery as a boolean gate: only select items whose supplier has positive balance.

    x <= (SELECT 1 FROM supplier WHERE s_suppkey = ps_suppkey AND s_acctbal > 0)
    effectively filters which rows can have x=1.
    """
    sql = """
        SELECT ps_partkey, ps_suppkey, ps_supplycost, x
        FROM partsupp
        WHERE ps_partkey < 10
        DECIDE x IS BOOLEAN
        SUCH THAT x <= COALESCE((SELECT 1 FROM supplier WHERE s_suppkey = ps_suppkey AND s_acctbal > 0), 0)
          AND SUM(x * ps_supplycost) <= 2000
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(ps_partkey AS BIGINT),
               CAST(ps_suppkey AS BIGINT),
               CAST(ps_supplycost AS DOUBLE),
               CASE WHEN s_acctbal > 0 THEN 1.0 ELSE 0.0 END AS eligible
        FROM partsupp
        LEFT JOIN supplier ON s_suppkey = ps_suppkey
        WHERE ps_partkey < 10
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("correlated_subquery_boolean")
    vnames = [f"x_{i}" for i in range(len(data))]
    for i, vn in enumerate(vnames):
        oracle_solver.add_variable(vn, VarType.BINARY, ub=data[i][3])

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 2000.0, name="budget",
    )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        packdb_result, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "correlated_subquery_boolean", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
