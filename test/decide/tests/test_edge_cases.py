"""Edge-case tests for DECIDE.

Covers boundary conditions that other test files don't exercise:
  - Single-row input (trivial solve)
  - Very loose constraint (all variables selected)
  - Constraint RHS = 0 (forces all decision vars to 0)
  - Negative objective coefficients
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_single_row(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Degenerate case: only 1 input row. Trivial knapsack."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey = 1 AND l_linenumber = 1
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0
    assert len(packdb_result) == 1, f"Expected 1 row, got {len(packdb_result)}"

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey = 1 AND l_linenumber = 1
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("single_row")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 100.0, name="capacity",
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
        "single_row", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_trivial_all_selected(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Constraint so loose every x=1 is feasible. Optimal is all-ones."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 20
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 999999
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
        FROM lineitem WHERE l_orderkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("trivial_all")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 999999.0, name="capacity",
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
    assert all(v == 1.0 for v in cmp.packdb_vector), "Expected all x=1"

    perf_tracker.record(
        "trivial_all_selected", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_rhs_zero_forces_all_zero(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(x) <= 0 forces all boolean variables to 0."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey < 20
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 0
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 20
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("rhs_zero")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 0.0, name="zero_budget",
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
    assert all(v == 0.0 for v in cmp.packdb_vector), "Expected all x=0"
    assert cmp.packdb_objective == 0.0

    perf_tracker.record(
        "rhs_zero", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.edge_case
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_negative_objective_coefficients(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Negative account balances as coefficients — solver must pick most negative."""
    sql = """
        SELECT c_custkey, c_acctbal, x
        FROM customer
        WHERE c_nationkey <= 3
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 5
        MINIMIZE SUM(x * c_acctbal)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(c_custkey AS BIGINT),
               CAST(c_acctbal AS DOUBLE)
        FROM customer WHERE c_nationkey <= 3
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("neg_coeffs")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        ">=", 5.0, name="min_count",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        packdb_result, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("c_acctbal")])},
    )

    perf_tracker.record(
        "neg_coeffs", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
