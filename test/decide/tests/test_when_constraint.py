"""Tests for WHEN clause on aggregate constraints.

Covers:
  - String equality filter on aggregate constraint
  - Multiple WHEN constraints with different categories
  - Numeric comparison in WHEN condition
  - Boundary: all rows match (equivalent to no WHEN)
  - Boundary: no rows match (constraint is trivially satisfied)
  - Mixed conditional + unconditional constraints
  - Constant coefficient in SUM with WHEN
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_aggregate_string_equality(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Capacity limit on returned items only: SUM(x*qty) <= 100 WHEN returnflag='R'."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'R'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_agg_str_eq")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # WHEN filter: only 'R' rows contribute to the capacity constraint
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'R'},
        "<=", 100.0, name="capacity_R",
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
        "when_agg_str_eq", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_multiple_categories(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Different capacity limits per category using multiple WHEN constraints."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 50 WHEN l_returnflag = 'R'
            AND SUM(x * l_quantity) <= 80 WHEN l_returnflag = 'A'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_multi_cat")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Two separate WHEN-filtered constraints
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'R'},
        "<=", 50.0, name="capacity_R",
    )
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'A'},
        "<=", 80.0, name="capacity_A",
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
        "when_multi_cat", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_aggregate_numeric_comparison(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Capacity limit on discounted items: SUM(x*price) <= 5000 WHEN discount >= 0.06."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_discount, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_extendedprice) <= 5000 WHEN l_discount >= 0.06
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               CAST(l_discount AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_agg_numeric")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # WHEN filter: only rows with discount >= 0.06
    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(len(data)) if data[i][4] >= 0.06},
        "<=", 5000.0, name="price_cap_discounted",
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
        "when_agg_numeric", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_all_rows_match(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN condition matches all rows — equivalent to no WHEN."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_quantity > 0
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE)
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_all_match")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # All l_quantity > 0 in TPC-H, so this includes all rows
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][3] > 0},
        "<=", 100.0, name="capacity_all",
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
        "when_all_match", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_no_rows_match(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN condition matches no rows — constraint is trivially satisfied."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 100 WHEN l_returnflag = 'Z'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_no_match")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # No rows have returnflag='Z', so constraint dict is empty → trivially true
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'Z'},
        "<=", 100.0, name="capacity_none",
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
        "when_no_match", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.cons_multi
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_mixed_conditional_and_unconditional(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """One WHEN-filtered constraint + one unconditional constraint."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 50 WHEN l_returnflag = 'R'
            AND SUM(x) <= 20
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_mixed")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Conditional: capacity on R rows only
    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] == 'R'},
        "<=", 50.0, name="capacity_R",
    )
    # Unconditional: at most 20 items total
    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(len(data))},
        "<=", 20.0, name="count_limit",
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
        "when_mixed", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 2,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_aggregate_constant_coeff(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """Constant coefficient in SUM with WHEN: SUM(x*10) <= 200 WHEN mktsegment='AUTOMOBILE'."""
    sql = """
        SELECT c_custkey, c_acctbal, c_mktsegment, x
        FROM customer
        WHERE c_nationkey = 1
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * 10) <= 200 WHEN c_mktsegment = 'AUTOMOBILE'
        MAXIMIZE SUM(x * c_acctbal)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(c_custkey AS BIGINT),
               CAST(c_acctbal AS DOUBLE),
               c_mktsegment
        FROM customer WHERE c_nationkey = 1
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_const_coeff")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Constant coeff=10 only for AUTOMOBILE customers
    oracle_solver.add_constraint(
        {vnames[i]: 10.0 for i in range(len(data)) if data[i][2] == 'AUTOMOBILE'},
        "<=", 200.0, name="budget_auto",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    cmp = compare_solutions(
        packdb_result, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("c_acctbal")])},
    )

    perf_tracker.record(
        "when_const_coeff", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.when_constraint
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_not_equal(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    """WHEN with not-equal operator: SUM(x*qty) <= 80 WHEN returnflag <> 'N'."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem
        WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 80 WHEN l_returnflag <> 'N'
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_quantity AS DOUBLE),
               l_returnflag
        FROM lineitem WHERE l_orderkey < 100
    """).fetchall()

    t_build = time.perf_counter()
    oracle_solver.create_model("when_not_eq")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][3] for i in range(len(data)) if data[i][4] != 'N'},
        "<=", 80.0, name="capacity_not_N",
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
        "when_not_eq", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )
