"""Tests for IS REAL (continuous) decision variables.

Covers:
  - test_real_basic: simple LP with DECIDE x IS REAL
  - test_real_with_bounds: REAL variable with explicit upper bound
  - test_real_mixed: mixed BOOLEAN + REAL variables
  - test_real_with_when: REAL variable + WHEN conditional
  - test_real_with_per: REAL variable + PER grouping
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from comparison.compare import compare_solutions


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_real_basic(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Continuous variable: maximize weighted sum with aggregate constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS REAL
        SUCH THAT SUM(x * l_quantity) <= 500
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("real_basic")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)

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
        "real_basic", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_real_with_bounds(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """REAL variable with explicit upper bound constraint x <= 5."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS REAL
        SUCH THAT x <= 5
            AND SUM(x * l_extendedprice) <= 50000
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

    t_build = time.perf_counter()
    oracle_solver.create_model("real_with_bounds")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=5.0)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data))},
        "<=", 50000.0, name="budget",
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
        "real_with_bounds", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_real_mixed(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Mixed BOOLEAN + REAL variables in same query."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, s, w
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE s IS BOOLEAN, w IS REAL
        SUCH THAT SUM(s * l_quantity) <= 50
            AND w <= 10
        MAXIMIZE SUM(s * l_extendedprice + w * l_quantity)
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
    oracle_solver.create_model("real_mixed")
    snames = [f"s_{i}" for i in range(len(data))]
    wnames = [f"w_{i}" for i in range(len(data))]
    for sn in snames:
        oracle_solver.add_variable(sn, VarType.BINARY)
    for wn in wnames:
        oracle_solver.add_variable(wn, VarType.CONTINUOUS, lb=0.0, ub=10.0)

    oracle_solver.add_constraint(
        {snames[i]: data[i][3] for i in range(len(data))},
        "<=", 50.0, name="capacity",
    )
    obj = {}
    for i in range(len(data)):
        obj[snames[i]] = data[i][2]  # s * extendedprice
        obj[wnames[i]] = data[i][3]  # w * quantity
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        packdb_result, packdb_cols, result, data, ["s", "w"],
        coeff_fn=lambda row: {
            "s": float(row[packdb_cols.index("l_extendedprice")]),
            "w": float(row[packdb_cols.index("l_quantity")]),
        },
    )

    perf_tracker.record(
        "real_mixed", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(snames) + len(wnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.when_constraint
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_real_with_when(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """REAL variable with WHEN conditional on constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, l_returnflag, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS REAL
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'R'
            AND x <= 10
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("real_with_when")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=10.0)

    # WHEN l_returnflag = 'R': only those rows participate in the constraint
    r_rows = {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'R'}
    if r_rows:
        oracle_solver.add_constraint(r_rows, "<=", 100.0, name="when_capacity")

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
        "real_with_when", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.var_real
@pytest.mark.per_clause
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_real_with_per(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """REAL variable with PER grouping constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS REAL
        SUCH THAT SUM(x * l_quantity) <= 50 PER l_orderkey
        MAXIMIZE SUM(x * l_extendedprice)
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

    # Group by l_orderkey for PER constraint
    groups = {}
    for i, row in enumerate(data):
        groups.setdefault(row[0], []).append(i)

    t_build = time.perf_counter()
    oracle_solver.create_model("real_with_per")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)

    for orderkey, row_indices in groups.items():
        oracle_solver.add_constraint(
            {vnames[i]: data[i][3] for i in row_indices},
            "<=", 50.0, name=f"per_{orderkey}",
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
        "real_with_per", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
