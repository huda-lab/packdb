"""Tests for COUNT(x) over INTEGER decision variables.

Uses Big-M indicator variables: for each INTEGER var x, introduces binary z
with z <= x and x <= M*z, then rewrites COUNT(x) to SUM(z). The oracle
mirrors the same rewrite so its objective/constraint shape matches PackDB's
compiled ILP.

Covers:
  - test_count_integer_constraint / _upper / _dedup: COUNT bounds
  - test_count_integer_objective: MAXIMIZE COUNT
  - test_count_integer_objective_minimize: MINIMIZE COUNT (indicator orientation
    — solver must be free to drive z_i to 0, which drives x_i to 0)
  - test_count_integer_hidden_indicator: indicator vars not in SELECT *
  - test_count_integer_with_when: COUNT(x) WHEN condition
"""

import time

import pytest

from solver.types import VarType, ObjSense
from comparison.compare import compare_solutions
from ._oracle_helpers import add_count_integer_indicators


def _run_and_compare(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker,
    *, test_id, data_sql, decide_sql,
    value_col_index, big_M, count_constraint=None, count_objective_max=False,
    per_group_fn=None, when_mask_fn=None,
    extra_constraints_fn=None,
    upper_count_constraint=None,
):
    """Drive PackDB + oracle and assert equivalence for a COUNT(INTEGER) test.

    Shared plumbing for the inline-VALUES COUNT tests in this file. Passing
    lambdas keeps each test declarative while funnelling the repetitive
    oracle build through a single code path.
    """
    t0 = time.perf_counter()
    packdb_rows, packdb_cols = packdb_cli.execute(decide_sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(data_sql).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model(test_id)
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=big_M)
    zs = add_count_integer_indicators(oracle_solver, vnames, big_M=big_M)

    if count_constraint is not None:
        sense, rhs = count_constraint
        oracle_solver.add_constraint(
            {z: 1.0 for z in zs}, sense, float(rhs), name="count_lb",
        )
    if upper_count_constraint is not None:
        sense, rhs = upper_count_constraint
        oracle_solver.add_constraint(
            {z: 1.0 for z in zs}, sense, float(rhs), name="count_ub",
        )
    if per_group_fn is not None:
        groups: dict = {}
        for i, row in enumerate(data):
            k = per_group_fn(row)
            if k is None:
                continue
            groups.setdefault(k, []).append(i)
        sense, rhs = count_constraint  # reused for per-group
        for key, idxs in groups.items():
            oracle_solver.add_constraint(
                {zs[i]: 1.0 for i in idxs}, sense, float(rhs),
                name=f"count_per_{key}",
            )
    if when_mask_fn is not None:
        mask = [1.0 if when_mask_fn(row) else 0.0 for row in data]
        sense, rhs = count_constraint
        oracle_solver.add_constraint(
            {zs[i]: mask[i] for i in range(n) if mask[i]},
            sense, float(rhs), name="count_when",
        )
    if extra_constraints_fn is not None:
        extra_constraints_fn(oracle_solver, vnames, zs, data)

    if count_objective_max:
        oracle_solver.set_objective(
            {z: 1.0 for z in zs}, ObjSense.MAXIMIZE,
        )
    else:
        oracle_solver.set_objective(
            {vnames[i]: data[i][value_col_index] for i in range(n)},
            ObjSense.MAXIMIZE,
        )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    if count_objective_max:
        # Non-linear objective: route `packdb_objective_fn` through
        # `compare_solutions` so the x-vector is still validated element-wise
        # against the oracle (not just the scalar count — an off-orientation
        # indicator could produce the same count with a wrong x-vector).
        def _packdb_count(rows, cols, _vcol=value_col_index):
            return float(
                sum(1 for r in rows if float(r[cols.index("x")]) > 0)
            )

        cmp = compare_solutions(
            packdb_rows, packdb_cols, result, data, ["x"],
            coeff_fn=lambda row: {"x": 0.0},
            packdb_objective_fn=_packdb_count,
        )
        perf_tracker.record(
            test_id, packdb_time, build_time, result.solve_time_seconds,
            n, 2 * n, 1,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status=cmp.status,
            decide_vector=cmp.oracle_vector,
        )
    else:
        cmp = compare_solutions(
            packdb_rows, packdb_cols, result, data, ["x"],
            coeff_fn=lambda row: {"x": float(row[value_col_index])},
        )
        perf_tracker.record(
            test_id, packdb_time, build_time, result.solve_time_seconds,
            n, 2 * n, 1,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status=cmp.status,
            decide_vector=cmp.oracle_vector,
        )


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_constraint(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """COUNT(x) >= k forces at least k rows to have x > 0."""
    data_sql = (
        "SELECT * FROM (VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)) t(name, value)"
    )
    decide_sql = f"""
        SELECT name, value, x FROM ({data_sql})
        DECIDE x
        SUCH THAT x <= 10 AND COUNT(x) >= 3
        MAXIMIZE SUM(x * value)
    """
    _run_and_compare(
        packdb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="count_integer_constraint",
        data_sql=data_sql, decide_sql=decide_sql,
        value_col_index=1, big_M=10.0,
        count_constraint=(">=", 3),
    )


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_constraint_upper(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """COUNT(x) <= k forces at most k rows to have x > 0."""
    data_sql = (
        "SELECT * FROM (VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)) t(name, value)"
    )
    decide_sql = f"""
        SELECT name, value, x FROM ({data_sql})
        DECIDE x
        SUCH THAT x <= 10 AND COUNT(x) <= 2
        MAXIMIZE SUM(x * value)
    """
    _run_and_compare(
        packdb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="count_integer_constraint_upper",
        data_sql=data_sql, decide_sql=decide_sql,
        value_col_index=1, big_M=10.0,
        upper_count_constraint=("<=", 2),
    )


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_objective(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MAXIMIZE COUNT(x) should maximize number of non-zero assignments."""
    data_sql = (
        "SELECT * FROM (VALUES ('a', 10), ('b', 5), ('c', 8)) t(name, value)"
    )
    decide_sql = f"""
        SELECT name, value, x FROM ({data_sql})
        DECIDE x
        SUCH THAT x <= 5 AND SUM(x) <= 10
        MAXIMIZE COUNT(x)
    """

    def extra(oracle, vnames, zs, data):
        oracle.add_constraint(
            {v: 1.0 for v in vnames}, "<=", 10.0, name="sum_cap",
        )

    _run_and_compare(
        packdb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="count_integer_objective",
        data_sql=data_sql, decide_sql=decide_sql,
        value_col_index=1, big_M=5.0,
        count_objective_max=True,
        extra_constraints_fn=extra,
    )


@pytest.mark.count_rewrite
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_count_integer_objective_minimize(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """MINIMIZE COUNT(x) should minimize number of non-zero assignments.

    Indicator orientation is the concern: ``z_i = 1 ⇔ x_i > 0``. Under
    MAXIMIZE COUNT the solver pushes z up (and must be able to); under
    MINIMIZE COUNT the solver pushes z down (and must be free to set z=0
    only when x=0). A wrong orientation would either force all z=1 or
    would allow x>0 with z=0, silently inflating or deflating the count.

    SUM(x) >= 12 with x <= 10 per row forces at least 2 non-zeros, so the
    oracle-optimal count is 2.
    """
    data_sql = (
        "SELECT * FROM (VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)) t(name, value)"
    )
    decide_sql = f"""
        SELECT name, value, x FROM ({data_sql})
        DECIDE x
        SUCH THAT SUM(x) >= 12 AND x <= 10
        MINIMIZE COUNT(x)
    """
    t0 = time.perf_counter()
    packdb_rows, packdb_cols = packdb_cli.execute(decide_sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute(data_sql).fetchall()
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("count_integer_objective_minimize")
    vnames = [f"x_{i}" for i in range(n)]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0.0, ub=10.0)
    zs = add_count_integer_indicators(oracle_solver, vnames, big_M=10.0)

    oracle_solver.add_constraint(
        {vnames[i]: 1.0 for i in range(n)}, ">=", 12.0, name="sum_floor",
    )
    oracle_solver.set_objective(
        {z: 1.0 for z in zs}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    # Non-linear COUNT objective: feed x-vector through compare_solutions via
    # `packdb_objective_fn`. Bare scalar-count comparison would silently pass
    # an incorrect x-vector that happens to hit the same count.
    def _packdb_count(rows, cols):
        return float(
            sum(1 for r in rows if float(r[cols.index("x")]) > 0)
        )

    cmp = compare_solutions(
        packdb_rows, packdb_cols, result, data, ["x"],
        coeff_fn=lambda row: {"x": 0.0},
        packdb_objective_fn=_packdb_count,
    )

    perf_tracker.record(
        "count_integer_objective_minimize", packdb_time, build_time,
        result.solve_time_seconds, n, 2 * n, 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status=cmp.status,
        decide_vector=cmp.oracle_vector,
    )


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_hidden_indicator(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Indicator variables should not appear in SELECT * output, and the
    objective value must match the oracle."""
    data_sql = "SELECT * FROM (VALUES ('a', 10), ('b', 5)) t(name, value)"
    decide_sql = f"""
        SELECT * FROM ({data_sql})
        DECIDE x
        SUCH THAT x <= 5 AND COUNT(x) >= 1
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(decide_sql)
    for col in cols:
        assert "__count_ind_" not in col, \
            f"Indicator variable '{col}' should be hidden from output"
    assert "x" in cols, "Decision variable x should be in output"

    _run_and_compare(
        packdb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="count_integer_hidden_indicator",
        data_sql=data_sql, decide_sql=decide_sql,
        value_col_index=1, big_M=5.0,
        count_constraint=(">=", 1),
    )


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_with_when(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """COUNT(x) WHEN condition should only count non-zero x among qualifying rows."""
    data_sql = (
        "SELECT * FROM (VALUES "
        "('a', 10, 'high'), ('b', 5, 'low'), ('c', 8, 'high'), ('d', 3, 'low')"
        ") t(name, value, tier)"
    )
    decide_sql = f"""
        SELECT name, value, tier, x FROM ({data_sql})
        DECIDE x
        SUCH THAT x <= 10
            AND COUNT(x) >= 1 WHEN tier = 'low'
        MAXIMIZE SUM(x * value)
    """
    _run_and_compare(
        packdb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="count_integer_with_when",
        data_sql=data_sql, decide_sql=decide_sql,
        value_col_index=1, big_M=10.0,
        count_constraint=(">=", 1),
        when_mask_fn=lambda row: row[2] == "low",
    )


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_dedup(
    packdb_cli, duckdb_conn, oracle_solver, perf_tracker
):
    """Multiple COUNT(x) references to same var should reuse one indicator.

    Oracle-side we encode both COUNT bounds as SUM(z) constraints; PackDB is
    expected to dedupe into a single set of indicator variables, but that's
    an internal optimization invisible at the objective/vector level.
    """
    data_sql = (
        "SELECT * FROM (VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)) t(name, value)"
    )
    decide_sql = f"""
        SELECT name, value, x FROM ({data_sql})
        DECIDE x
        SUCH THAT x <= 10 AND COUNT(x) >= 2 AND COUNT(x) <= 3
        MAXIMIZE SUM(x * value)
    """
    _run_and_compare(
        packdb_cli, duckdb_conn, oracle_solver, perf_tracker,
        test_id="count_integer_dedup",
        data_sql=data_sql, decide_sql=decide_sql,
        value_col_index=1, big_M=10.0,
        count_constraint=(">=", 2),
        upper_count_constraint=("<=", 3),
    )
