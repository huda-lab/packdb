"""Tests for IN (...) constraints on decision variables.

``x IN (v1, ..., vK)`` restricts the variable's domain to a discrete set.
PackDB rewrites this at bind time into K binary indicator variables z_k with
``SUM(z_k) = 1`` (cardinality) and ``x = SUM(v_k * z_k)`` (linking). The oracle
mirrors the same construction via ``add_in_domain``.
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions
from ._oracle_helpers import add_in_domain


def _common_lineitem(duckdb_conn, where_clause):
    return duckdb_conn.execute(f"""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE {where_clause}
    """).fetchall()


def _build_in_domain_model(oracle, test_id, data, domain, big_M,
                           when_mask=None, explicit_bool=False):
    """Create vars for each row, apply IN (domain), return var name list."""
    oracle.create_model(test_id)
    n = len(data)
    vnames = [f"x_{i}" for i in range(n)]
    for i, v in enumerate(vnames):
        if explicit_bool:
            oracle.add_variable(v, VarType.BINARY)
        else:
            oracle.add_variable(v, VarType.INTEGER, lb=0.0, ub=big_M)
    if not explicit_bool:
        for i, v in enumerate(vnames):
            if when_mask is None or when_mask[i]:
                add_in_domain(oracle, v, domain, name_prefix=f"in_{i}")
    return vnames


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_in_domain_restriction(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (0, 1, 3) — restrict integer variable to a sparse domain."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (0, 1, 3)
            AND SUM(x * l_quantity) <= 200
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_rows, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_domain_restriction", data, [0, 1, 3], big_M=3.0,
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(n)},
        "<=", 200.0, name="quantity_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = packdb_cols.index("x")
    for row in packdb_rows:
        assert row[x_idx] in (0, 1, 3), f"x={row[x_idx]} not in allowed domain"

    cmp = compare_solutions(
        packdb_rows, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "in_domain_restriction", packdb_time, build_time, result.solve_time_seconds,
        n, n * 4, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_in_binary_domain(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (0, 1) on an implicitly typed variable — equivalent to IS BOOLEAN."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (0, 1)
            AND SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_rows, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_binary_domain", data, [0, 1], big_M=1.0,
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(n)},
        "<=", 100.0, name="quantity_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = packdb_cols.index("x")
    for row in packdb_rows:
        assert row[x_idx] in (0, 1), f"x={row[x_idx]} not in {{0, 1}}"

    cmp = compare_solutions(
        packdb_rows, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "in_binary_domain", packdb_time, build_time, result.solve_time_seconds,
        n, n * 3, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_in_single_value(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (3) — single-value IN, equivalent to x = 3."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (3)
        MINIMIZE SUM(x * l_quantity)
    """
    t0 = time.perf_counter()
    packdb_rows, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_single_value", data, [3], big_M=3.0,
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = packdb_cols.index("x")
    for row in packdb_rows:
        assert row[x_idx] == 3, f"x={row[x_idx]}, expected 3"

    cmp = compare_solutions(
        packdb_rows, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_quantity")])},
    )
    perf_tracker.record(
        "in_single_value", packdb_time, build_time, result.solve_time_seconds,
        n, n * 2, 0, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_in_minimize_picks_smallest(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (3, 5, 7) MINIMIZE SUM(x) — solver should pick 3 for all rows."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (3, 5, 7)
        MINIMIZE SUM(x * l_quantity)
    """
    t0 = time.perf_counter()
    packdb_rows, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_minimize_picks_smallest", data, [3, 5, 7], big_M=7.0,
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = packdb_cols.index("x")
    for row in packdb_rows:
        assert row[x_idx] in (3, 5, 7)
        assert row[x_idx] == 3, f"MINIMIZE should pick smallest: x={row[x_idx]}"

    cmp = compare_solutions(
        packdb_rows, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_quantity")])},
    )
    perf_tracker.record(
        "in_minimize_picks_smallest", packdb_time, build_time, result.solve_time_seconds,
        n, n * 4, 0, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_in_maximize_picks_largest(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (2, 5) MAXIMIZE SUM(x * extendedprice) with a loose aggregate cap."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (2, 5)
            AND SUM(x * l_quantity) <= 99999
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_rows, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_maximize_picks_largest", data, [2, 5], big_M=5.0,
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(n)},
        "<=", 99999.0, name="quantity_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = packdb_cols.index("x")
    for row in packdb_rows:
        assert row[x_idx] in (2, 5), f"x={row[x_idx]} not in domain"

    cmp = compare_solutions(
        packdb_rows, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "in_maximize_picks_largest", packdb_time, build_time, result.solve_time_seconds,
        n, n * 3, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.when
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_in_with_when(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IN (0, 2, 4) WHEN condition — IN restriction gated by a row filter."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (0, 2, 4) WHEN l_quantity > 20
            AND SUM(x * l_quantity) <= 500
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_rows, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)
    mask = [row[3] > 20 for row in data]

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_with_when", data, [0, 2, 4], big_M=4.0,
        when_mask=mask,
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(n)},
        "<=", 500.0, name="quantity_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = packdb_cols.index("x")
    qty_idx = packdb_cols.index("l_quantity")
    for row in packdb_rows:
        if row[qty_idx] > 20:
            assert row[x_idx] in (0, 2, 4), \
                f"x={row[x_idx]} not in domain when l_quantity={row[qty_idx]} > 20"

    cmp = compare_solutions(
        packdb_rows, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "in_with_when", packdb_time, build_time, result.solve_time_seconds,
        n, n * 4, 2, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )


@pytest.mark.cons_in
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_in_boolean_explicit(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """x IS BOOLEAN with x IN (0, 1) — trivially satisfied, no auxiliary vars needed."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT x IN (0, 1)
            AND SUM(x) <= 10
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_rows, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = _common_lineitem(duckdb_conn, "l_orderkey < 50")
    n = len(data)

    t_build = time.perf_counter()
    vnames = _build_in_domain_model(
        oracle_solver, "in_boolean_explicit", data, [0, 1], big_M=1.0,
        explicit_bool=True,
    )
    oracle_solver.add_constraint(
        {v: 1.0 for v in vnames}, "<=", 10.0, name="count_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][2] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    x_idx = packdb_cols.index("x")
    for row in packdb_rows:
        assert row[x_idx] in (0, 1), f"x={row[x_idx]} not in {{0, 1}}"

    cmp = compare_solutions(
        packdb_rows, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_extendedprice")])},
    )
    perf_tracker.record(
        "in_boolean_explicit", packdb_time, build_time, result.solve_time_seconds,
        n, n, 1, result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
    )
