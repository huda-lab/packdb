"""Tests for COUNT(x) → SUM(x) rewrite on BOOLEAN decision variables.

Covers:
  - test_count_constraint: COUNT(x) in SUCH THAT constraint
  - test_count_objective: COUNT(x) as MAXIMIZE objective
  - test_count_with_when: COUNT(x) with WHEN conditional
  - test_count_with_per: COUNT(x) with PER grouping
  - test_count_equivalence: verify COUNT(x) == SUM(x) on same problem
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from comparison.compare import compare_solutions


@pytest.mark.count_rewrite
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_count_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """COUNT(x) >= 5 in constraint where x IS BOOLEAN."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT COUNT(x) >= 5
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("count_constraint")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # COUNT(x) >= 5 is the same as SUM(x) >= 5 for BOOLEAN
    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames},
        ">=", 5.0, name="count_ge_5",
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
        "count_constraint", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.count_rewrite
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_count_objective(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAXIMIZE COUNT(x) where x IS BOOLEAN — maximize number of selected rows."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE COUNT(x)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("count_objective")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 100.0, name="capacity",
    )
    # MAXIMIZE COUNT(x) == MAXIMIZE SUM(x) for BOOLEAN
    oracle_solver.set_objective(
        {vn: 1.0 for vn in vnames},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    # Coefficient for COUNT objective is 1.0 per row
    cmp = compare_solutions(
        packdb_result, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 1.0},
    )

    perf_tracker.record(
        "count_objective", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.count_rewrite
@pytest.mark.var_boolean
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_count_with_when(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """COUNT(x) >= 3 WHEN l_returnflag = 'R'."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT COUNT(x) >= 3 WHEN l_returnflag = 'R'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("count_with_when")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # WHEN l_returnflag = 'R': only those rows in constraint
    r_rows = {vnames[i]: 1.0 for i in range(len(data)) if data[i][3] == 'R'}
    if r_rows:
        oracle_solver.add_constraint(r_rows, ">=", 3.0, name="count_when_R")

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
        "count_with_when", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.count_rewrite
@pytest.mark.var_boolean
@pytest.mark.per_clause
@pytest.mark.correctness
def test_count_with_per(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """COUNT(x) >= 1 PER l_orderkey."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT COUNT(x) >= 1 PER l_orderkey
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    groups = {}
    for i, row in enumerate(data):
        groups.setdefault(row[0], []).append(i)

    t_build = time.perf_counter()
    oracle_solver.create_model("count_with_per")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    for orderkey, row_indices in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: 1.0 for i in row_indices},
            ">=", 1.0, name=f"count_per_{orderkey}",
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
        "count_with_per", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.count_rewrite
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_count_equivalence(packdb_cli):
    """Verify COUNT(x) and SUM(x) produce identical results on same problem."""
    base_sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT {agg}(x) >= 3
        MAXIMIZE SUM(x * l_extendedprice)
    """
    count_result, count_cols = packdb_cli.execute(base_sql.format(agg="COUNT"))
    sum_result, sum_cols = packdb_cli.execute(base_sql.format(agg="SUM"))

    assert count_cols == sum_cols, "Column names should match"
    assert len(count_result) == len(sum_result), "Row counts should match"

    # Sort both by non-decision columns for deterministic comparison
    key_indices = [i for i, c in enumerate(count_cols) if c != "x"]
    sort_key = lambda row: tuple(row[i] for i in key_indices)

    count_sorted = sorted(count_result, key=sort_key)
    sum_sorted = sorted(sum_result, key=sort_key)

    for cr, sr in zip(count_sorted, sum_sorted):
        for i in range(len(cr)):
            assert abs(float(cr[i]) - float(sr[i])) < 1e-6, (
                f"Results differ: COUNT row {cr} vs SUM row {sr}"
            )
