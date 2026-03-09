"""Tests for comparison operators on aggregate constraints.

<= and >= are exercised heavily in other test files. This file covers
the remaining operators:
  - SUM equality: SUM(x) = value
  - Strict less-than: SUM(x * col) < value
  - Strict greater-than: SUM(x) > value
  - Not-equal: SUM(x) <> value
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_equality_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(x) = exact_value — fix the total count of selected items."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) = 10
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
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_eq")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "=", 10.0, name="exact_count",
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
        "sum_eq", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_strict_less_than(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(x * qty) < 100 — strict less-than on aggregate."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) < 100
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
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_strict_lt")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # For integer variables, SUM < 100 ≡ SUM <= 99
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 99.0, name="capacity_strict",
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
        "sum_strict_lt", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_sum_strict_greater_than(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(x) > 5 — strict greater-than on aggregate."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) > 5
            AND SUM(x * l_quantity) <= 200
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
        FROM lineitem WHERE l_orderkey < 50
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("sum_strict_gt")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # For integer variables, SUM(x) > 5 ≡ SUM(x) >= 6
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        ">=", 6.0, name="min_count_strict",
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data))},
        "<=", 200.0, name="capacity",
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
        "sum_strict_gt", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
def test_sum_not_equal(packdb_cli):
    """SUM(x) <> 5 — not-equal on aggregate (disjunctive, hard for ILP)."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <> 5
            AND SUM(x * l_quantity) <= 200
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    total = sum(row[4] for row in result)
    assert total != 5, f"SUM(x) = {total}, expected <> 5"


@pytest.mark.cons_comparison
@pytest.mark.var_integer
@pytest.mark.obj_maximize
def test_perrow_not_equal(packdb_cli):
    """x <> 3 — per-row not-equal constraint."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x <> 3
            AND x <= 5
            AND SUM(x * l_quantity) <= 500
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    x_idx = 2
    for row in result:
        assert row[x_idx] != 3, f"x={row[x_idx]}, expected <> 3"


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
def test_sum_not_equal_zero(packdb_cli):
    """SUM(x) <> 0 — forces at least one selected."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <> 0
            AND SUM(x) <= 3
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    total = sum(row[2] for row in result)
    assert total != 0, f"SUM(x) = {total}, expected <> 0"
    assert total >= 1, f"SUM(x) must be at least 1"


@pytest.mark.cons_comparison
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.when
@pytest.mark.obj_maximize
def test_sum_not_equal_with_when(packdb_cli):
    """SUM(x) <> 5 WHEN condition — NE with conditional application."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <> 5 WHEN l_quantity > 20
            AND SUM(x * l_quantity) <= 200
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    conditional_total = sum(row[3] for row in result if row[2] > 20)
    assert conditional_total != 5, \
        f"SUM(x) WHEN l_quantity > 20 = {conditional_total}, expected <> 5"
