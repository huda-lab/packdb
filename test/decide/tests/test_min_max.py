"""Tests for MIN/MAX aggregate linearization over decision variables.

Covers:
  Easy constraint cases:
  - test_max_leq_constraint: MAX(x) <= K → per-row x <= K
  - test_min_geq_constraint: MIN(x) >= K → per-row x >= K
  - test_max_leq_with_expr: MAX(x * col) <= K

  Hard constraint cases:
  - test_max_geq_constraint: MAX(x) >= K → indicator + Big-M
  - test_min_leq_constraint: MIN(x) <= K → indicator + Big-M
  - test_max_eq_constraint: MAX(x) = K → easy + hard combined
  - test_min_eq_constraint: MIN(x) = K → easy + hard combined

  Easy objective cases:
  - test_minimize_max_objective: MINIMIZE MAX(x * cost)
  - test_maximize_min_objective: MAXIMIZE MIN(x * cost)

  Hard objective cases:
  - test_maximize_max_objective: MAXIMIZE MAX(x * cost)
  - test_minimize_min_objective: MINIMIZE MIN(x * cost)

  Modifiers:
  - test_max_constraint_with_when: MAX(x) <= K WHEN condition
  - test_min_objective_with_when: MAXIMIZE MIN(x * cost) WHEN condition
  - test_max_constraint_with_per: MAX(x) <= K PER column
  - test_min_max_when_per_composition: WHEN + PER combined

  INTEGER variable tests:
  - test_max_leq_integer: MAX(x) <= K with INTEGER variables
  - test_minimize_max_integer: MINIMIZE MAX(x * col) with INTEGER variables

  Combined tests:
  - test_multiple_minmax_constraints: Multiple MIN/MAX constraints in same query
  - test_minmax_constraint_and_objective: MIN/MAX in both constraint and objective

  Error cases:
  - test_max_notequal_error: MAX(x) <> K should error
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus


# ============================================================================
# Easy Constraint Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_max_leq_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) <= 0 forces all x=0; MAX(x) <= 1 is trivially satisfied for BOOLEAN."""
    # MAX(x) <= 0 → each row: x <= 0 → all x = 0
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x) <= 0
        MAXIMIZE SUM(x)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    # All x should be 0
    ci = {name: i for i, name in enumerate(packdb_cols)}
    for row in packdb_result:
        assert int(row[ci["x"]]) == 0, f"Expected x=0, got {row[ci['x']]}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_min_geq_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN(x) >= 1 forces all x=1 for BOOLEAN."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MIN(x) >= 1
        MAXIMIZE SUM(x)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(packdb_cols)}
    for row in packdb_result:
        assert int(row[ci["x"]]) == 1, f"Expected x=1, got {row[ci['x']]}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_max_leq_with_expr(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x * l_quantity) <= threshold: per-row x * l_quantity <= threshold."""
    threshold = 25.0
    sql = f"""
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x * l_quantity) <= {threshold}
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    # Oracle: per-row x_i * qty_i <= threshold
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("max_leq_expr")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {vnames[i]: qty}, "<=", threshold, name=f"max_row_{i}",
        )
    oracle_solver.set_objective(
        {vnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL
    # Count selected items in PackDB result
    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_count = sum(1 for row in packdb_result if int(row[ci["x"]]) == 1)
    assert abs(packdb_count - result.objective_value) <= 0.5, (
        f"Objective mismatch: PackDB selected {packdb_count}, Oracle={result.objective_value:.0f}"
    )

    perf_tracker.record(
        "max_leq_expr", packdb_time, build_time,
        result.solve_time_seconds, n, n, n,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# Hard Constraint Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_max_geq_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) >= 1 means at least one x must be 1. MINIMIZE SUM(x)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x) >= 1
        MINIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    # Oracle: SUM(x) >= 1, MINIMIZE SUM(x) → exactly 1 selected
    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_count = sum(1 for row in packdb_result if int(row[ci["x"]]) == 1)
    assert packdb_count == 1, f"Expected exactly 1 selected, got {packdb_count}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_min_leq_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN(x) <= 0 means at least one x must be 0. MAXIMIZE SUM(x)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MIN(x) <= 0
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    ci = {name: i for i, name in enumerate(packdb_cols)}
    zero_count = sum(1 for row in packdb_result if int(row[ci["x"]]) == 0)
    assert zero_count >= 1, "MIN(x) <= 0 requires at least one x=0"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_max_eq_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) = 1 means at least one x=1 AND all x<=1 (trivial for BOOLEAN)."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x) = 1
        MINIMIZE SUM(x)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(packdb_cols)}
    selected = sum(1 for row in packdb_result if int(row[ci["x"]]) == 1)
    assert selected >= 1, "MAX(x) = 1 requires at least one x=1"
    # MAX(x) = 1 with MINIMIZE SUM(x) → exactly one selected
    assert selected == 1, f"Expected 1 selected (minimize), got {selected}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_min_eq_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN(x) = 0 means at least one x=0 AND all x>=0 (trivial for BOOLEAN)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MIN(x) = 0
        MAXIMIZE SUM(x * l_extendedprice)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(packdb_cols)}
    zeros = sum(1 for row in packdb_result if int(row[ci["x"]]) == 0)
    assert zeros >= 1, "MIN(x) = 0 requires at least one x=0"


# ============================================================================
# Easy Objective Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_max_objective(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE MAX(x * l_quantity): minimize worst-case quantity."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 2
        MINIMIZE MAX(x * l_quantity)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    # Oracle: minimize z, z >= x_i * qty_i for all i, SUM(x) >= 2
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("minimize_max")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0)

    # SUM(x) >= 2
    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, ">=", 2.0, name="sum_ge_2",
    )
    # z >= x_i * qty_i for each row
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -qty}, ">=", 0.0, name=f"z_ge_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    # Compute PackDB's MAX(x * qty)
    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_max = max(
        int(row[ci["x"]]) * float(row[ci["l_quantity"]])
        for row in packdb_result
    )
    assert abs(packdb_max - result.objective_value) <= 0.5, (
        f"Objective mismatch: PackDB MAX={packdb_max:.2f}, Oracle={result.objective_value:.2f}"
    )

    perf_tracker.record(
        "minimize_max", packdb_time, build_time,
        result.solve_time_seconds, n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_min_objective(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAXIMIZE MIN(x * l_quantity): maximize worst-case, all must be selected."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 2
        MAXIMIZE MIN(x * l_quantity)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 5
    """).fetchall()

    # Oracle: maximize z, z <= x_i * qty_i for all i, SUM(x) >= 2
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("maximize_min")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0)

    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, ">=", 2.0, name="sum_ge_2",
    )
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -qty}, "<=", 0.0, name=f"z_le_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_min = min(
        int(row[ci["x"]]) * float(row[ci["l_quantity"]])
        for row in packdb_result
    )
    assert abs(packdb_min - result.objective_value) <= 0.5, (
        f"Objective mismatch: PackDB MIN={packdb_min:.2f}, Oracle={result.objective_value:.2f}"
    )

    perf_tracker.record(
        "maximize_min", packdb_time, build_time,
        result.solve_time_seconds, n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# Hard Objective Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_maximize_max_objective(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAXIMIZE MAX(x * l_extendedprice) with SUM(x) <= 3."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 3
        MAXIMIZE MAX(x * l_extendedprice)
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

    # Oracle: maximize z, z <= x_i * price_i + M*(1-y_i), SUM(y)>=1, SUM(x)<=3
    # Or equivalently: just maximize the max selected price
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("maximize_max")
    vnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    M = max(d[2] for d in data) * 2  # Big enough M

    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.BINARY)
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0)

    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, "<=", 3.0, name="sum_le_3",
    )
    oracle_solver.add_constraint(
        {yn: 1.0 for yn in ynames}, ">=", 1.0, name="sum_y_ge_1",
    )
    for i in range(n):
        price = data[i][2]
        # z <= x_i * price + M*(1-y_i) → z - price*x_i + M*y_i <= M
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -price, ynames[i]: M}, "<=", M,
            name=f"link_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_max = max(
        int(row[ci["x"]]) * float(row[ci["l_extendedprice"]])
        for row in packdb_result
    )
    assert abs(packdb_max - result.objective_value) <= 1.0, (
        f"Objective mismatch: PackDB MAX={packdb_max:.2f}, Oracle={result.objective_value:.2f}"
    )

    perf_tracker.record(
        "maximize_max", packdb_time, build_time,
        result.solve_time_seconds, n, n * 2 + 1, n + 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# WHEN Modifier
# ============================================================================

@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_max_constraint_with_when(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) <= 0 WHEN l_quantity > 30: only rows with qty>30 must have x=0."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x) <= 0 WHEN l_quantity > 30
        MAXIMIZE SUM(x)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(packdb_cols)}
    for row in packdb_result:
        qty = float(row[ci["l_quantity"]])
        x_val = int(row[ci["x"]])
        if qty > 30:
            assert x_val == 0, f"Row with qty={qty} should have x=0, got {x_val}"


# ============================================================================
# Error Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.when_objective
@pytest.mark.correctness
def test_min_objective_with_when(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAXIMIZE MIN(x * l_quantity) WHEN l_quantity <= 30: only qualifying rows count."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 2
        MAXIMIZE MIN(x * l_quantity) WHEN l_quantity <= 30
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(packdb_cols)}
    selected = sum(1 for row in packdb_result if int(row[ci["x"]]) == 1)
    assert selected >= 2, f"SUM(x) >= 2 not satisfied: got {selected}"


# ============================================================================
# Hard Objective Cases (continued)
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_min_objective(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE MIN(x) with INTEGER vars, x >= 1, SUM(x) >= 10: hard objective, spread values low."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x IS INTEGER
        SUCH THAT x >= 1 AND
              x <= 5 AND
              SUM(x) >= 10
        MINIMIZE MIN(x)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT)
        FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()

    # Oracle: minimize z, z >= x_i - M*(1-y_i), SUM(y) >= 1, x_i in [1,5], SUM(x) >= 10
    n = len(data)
    M = 10  # upper bound - lower bound
    t_build = time.perf_counter()
    oracle_solver.create_model("minimize_min")
    vnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]

    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=1, ub=5)
    for yn in ynames:
        oracle_solver.add_variable(yn, VarType.BINARY)
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=1.0, ub=5.0)

    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, ">=", 10.0, name="sum_ge_10",
    )
    oracle_solver.add_constraint(
        {yn: 1.0 for yn in ynames}, ">=", 1.0, name="sum_y_ge_1",
    )
    for i in range(n):
        # z >= x_i - M*(1-y_i) → z - x_i - M*y_i >= -M
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -1.0, ynames[i]: -M}, ">=", -M,
            name=f"link_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_min_val = min(int(row[ci["x"]]) for row in packdb_result)
    assert abs(packdb_min_val - result.objective_value) <= 0.5, (
        f"Objective mismatch: PackDB MIN={packdb_min_val}, Oracle={result.objective_value:.0f}"
    )

    perf_tracker.record(
        "minimize_min", packdb_time, build_time,
        result.solve_time_seconds, n, n * 2 + 1, n + 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# PER Modifier
# ============================================================================

@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_max_constraint_with_per(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) <= 0 PER l_orderkey: within each order, no item selected.
    Easy case with PER — PER is stripped (redundant since per-row already)."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x) <= 0 PER l_orderkey
        MAXIMIZE SUM(x)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    # MAX(x) <= 0 per group → all x = 0 (same as without PER for easy case)
    ci = {name: i for i, name in enumerate(packdb_cols)}
    for row in packdb_result:
        assert int(row[ci["x"]]) == 0, f"Expected x=0, got {row[ci['x']]}"


# ============================================================================
# WHEN + PER Composition
# ============================================================================

@pytest.mark.min_max
@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.var_boolean
@pytest.mark.correctness
def test_min_max_when_per_composition(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) <= 0 WHEN l_quantity > 40 PER l_orderkey: zero out high-qty items per order."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 10
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x) <= 0 WHEN l_quantity > 40 PER l_orderkey
        MAXIMIZE SUM(x)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(packdb_cols)}
    for row in packdb_result:
        qty = float(row[ci["l_quantity"]])
        x_val = int(row[ci["x"]])
        if qty > 40:
            assert x_val == 0, f"Row with qty={qty} should have x=0 (WHEN filter), got {x_val}"


# ============================================================================
# INTEGER Variable Tests
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_integer
@pytest.mark.cons_perrow
@pytest.mark.correctness
def test_max_leq_integer(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x) <= 3 with INTEGER variables: each x is bounded to [0, 3]."""
    sql = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x IS INTEGER
        SUCH THAT MAX(x) <= 3
        MAXIMIZE SUM(x)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(packdb_cols)}
    for row in packdb_result:
        x_val = int(row[ci["x"]])
        assert x_val <= 3, f"Expected x<=3, got {x_val}"
        # With MAXIMIZE SUM(x) and MAX(x) <= 3, each x should be 3
        assert x_val == 3, f"Expected x=3 (maximize), got {x_val}"


@pytest.mark.min_max
@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minimize_max_integer(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MINIMIZE MAX(x) with INTEGER: minimize the largest x value, with sum constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 3
        DECIDE x IS INTEGER
        SUCH THAT SUM(x) >= 10 AND
              x <= 5
        MINIMIZE MAX(x)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey <= 3
    """).fetchall()

    # Oracle: minimize z, z >= x_i for all i, SUM(x) >= 10, x_i <= 5
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("minimize_max_int")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0, ub=5)
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0)

    oracle_solver.add_constraint(
        {vn: 1.0 for vn in vnames}, ">=", 10.0, name="sum_ge_10",
    )
    for i in range(n):
        oracle_solver.add_constraint(
            {"z": 1.0, vnames[i]: -1.0}, ">=", 0.0, name=f"z_ge_{i}",
        )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_max = max(int(row[ci["x"]]) for row in packdb_result)
    assert abs(packdb_max - result.objective_value) <= 0.5, (
        f"Objective mismatch: PackDB MAX={packdb_max}, Oracle={result.objective_value:.0f}"
    )

    perf_tracker.record(
        "minimize_max_int", packdb_time, build_time,
        result.solve_time_seconds, n, n + 1, n + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# Combined MIN/MAX Tests
# ============================================================================

@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_multi
@pytest.mark.correctness
def test_multiple_minmax_constraints(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Multiple MIN/MAX constraints in same query: MAX(x) <= 0 WHEN ... AND MIN(x) >= 1 WHEN ..."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x) <= 0 WHEN l_quantity > 40 AND
              MIN(x) >= 1 WHEN l_quantity < 5
        MAXIMIZE SUM(x)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    ci = {name: i for i, name in enumerate(packdb_cols)}
    for row in packdb_result:
        qty = float(row[ci["l_quantity"]])
        x_val = int(row[ci["x"]])
        if qty > 40:
            assert x_val == 0, f"qty={qty}>40 should force x=0, got {x_val}"
        if qty < 5:
            assert x_val == 1, f"qty={qty}<5 should force x=1, got {x_val}"


@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_minmax_constraint_and_objective(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN/MAX in both constraint and objective: MAX(x) >= 1 with MINIMIZE MAX(x * price)."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, x
        FROM lineitem
        WHERE l_orderkey <= 5
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x) >= 1
        MINIMIZE MAX(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    ci = {name: i for i, name in enumerate(packdb_cols)}
    selected = [row for row in packdb_result if int(row[ci["x"]]) == 1]
    assert len(selected) >= 1, "MAX(x) >= 1 requires at least one x=1"

    # With MINIMIZE MAX(x*price) and MAX(x)>=1, should select 1 item with lowest price
    if len(selected) == 1:
        sel_price = float(selected[0][ci["l_extendedprice"]])
        all_prices = [float(row[ci["l_extendedprice"]]) for row in packdb_result]
        min_price = min(all_prices)
        assert abs(sel_price - min_price) <= 0.01, (
            f"Should select cheapest item: got {sel_price}, min is {min_price}"
        )


# ============================================================================
# Error Cases
# ============================================================================

@pytest.mark.min_max
@pytest.mark.error
@pytest.mark.error_binder
def test_max_notequal_error(packdb_cli):
    """MAX(x) <> K should produce a binder error."""
    packdb_cli.assert_error("""
        SELECT l_quantity FROM lineitem
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x) <> 0
        MAXIMIZE SUM(x) LIMIT 1
    """, match=r"does not support <> comparison with MIN/MAX")
