"""Tests for PER STRICT keyword.

PER STRICT switches from WHEN→PER (default: skip empty groups) to PER→WHEN
(evaluate all groups). Empty groups emit constraints with ``AGG(∅)``:
  SUM(∅) = 0, MAX(∅) = -∞, MIN(∅) = +∞.

The oracle mirrors that by iterating over *all* distinct PER values present
in the pre-WHEN data. For each value, the WHEN-qualifying subset drives the
constraint; if empty, the constraint shape (SUM/MIN/MAX + sense) decides
whether it's vacuously true or forces infeasibility.
"""

import time
from collections import defaultdict

import pytest

from solver.types import VarType, ObjSense, SolverStatus
from comparison.compare import compare_solutions
from ._oracle_helpers import add_ne_indicator


# ---------------------------------------------------------------------------
# Shared helpers for PER STRICT oracle construction.
# ---------------------------------------------------------------------------

def _per_strict_sum(
    oracle, var_names, data, *, key_fn, mask_fn,
    sense, rhs, name_prefix,
):
    """Emit one SUM(coeff * x) constraint per distinct PER value.

    Empty groups (where mask_fn is False for every row with that key) emit
    the constraint anyway with an empty LinExpr; Gurobi accepts that and
    the constraint collapses to ``0 [sense] rhs``.
    """
    groups: dict[object, list[int]] = defaultdict(list)
    for i, row in enumerate(data):
        k = key_fn(row)
        if k is None:
            continue
        groups[k].append(i)
    for key, idxs in groups.items():
        qualifying = [i for i in idxs if mask_fn(data[i])]
        coeffs = {var_names[i]: 1.0 for i in qualifying}
        oracle.add_constraint(coeffs, sense, float(rhs), name=f"{name_prefix}_{key}")


def _per_strict_max_hard(
    oracle, var_names, data, *, key_fn, mask_fn, rhs, name_prefix,
):
    """MAX(x) >= rhs hard-case per-group linearization for PER STRICT.

    For each PER group, introduce a binary y_i for every qualifying row:
      y_i = 1  ⇒  x_i >= rhs   (indicator constraint)
      SUM(y_i) >= 1             (at least one row achieves the max)

    Empty groups emit a zero-term SUM(y_i) >= 1 → infeasible, matching
    ``MAX(∅) = -∞ ≥ rhs`` semantics.
    """
    groups: dict[object, list[int]] = defaultdict(list)
    for i, row in enumerate(data):
        k = key_fn(row)
        if k is None:
            continue
        groups[k].append(i)
    for key, idxs in groups.items():
        qualifying = [i for i in idxs if mask_fn(data[i])]
        y_names = []
        for i in qualifying:
            y = f"y_{name_prefix}_{key}_{i}"
            oracle.add_variable(y, VarType.BINARY)
            oracle.add_indicator_constraint(
                y, 1, {var_names[i]: 1.0}, ">=", rhs, name=f"{y}_imp",
            )
            y_names.append(y)
        oracle.add_constraint(
            {y: 1.0 for y in y_names}, ">=", 1.0, name=f"{name_prefix}_max_ge_{key}",
        )


def _per_strict_min_hard(
    oracle, var_names, data, *, key_fn, mask_fn, rhs, name_prefix,
):
    """MIN(x) <= rhs hard-case per-group linearization for PER STRICT."""
    groups: dict[object, list[int]] = defaultdict(list)
    for i, row in enumerate(data):
        k = key_fn(row)
        if k is None:
            continue
        groups[k].append(i)
    for key, idxs in groups.items():
        qualifying = [i for i in idxs if mask_fn(data[i])]
        y_names = []
        for i in qualifying:
            y = f"y_{name_prefix}_{key}_{i}"
            oracle.add_variable(y, VarType.BINARY)
            oracle.add_indicator_constraint(
                y, 1, {var_names[i]: 1.0}, "<=", rhs, name=f"{y}_imp",
            )
            y_names.append(y)
        oracle.add_constraint(
            {y: 1.0 for y in y_names}, ">=", 1.0, name=f"{name_prefix}_min_le_{key}",
        )


def _fetch(duckdb_conn, sql):
    return duckdb_conn.execute(sql).fetchall()


_LINEITEM_COLS = (
    "CAST(l_orderkey AS BIGINT), CAST(l_linenumber AS BIGINT), "
    "CAST(l_returnflag AS VARCHAR), CAST(l_linestatus AS VARCHAR), "
    "CAST(l_quantity AS DOUBLE), CAST(l_extendedprice AS DOUBLE)"
)


# ---------------------------------------------------------------------------
# SUM + PER STRICT
# ---------------------------------------------------------------------------

@pytest.mark.per_strict
@pytest.mark.per_clause
@pytest.mark.correctness
class TestPerStrictConstraints:

    def test_sum_ge_empty_group_infeasible(
        self, packdb_cli, duckdb_conn, oracle_solver
    ):
        """SUM >= 1 with empty group: 0 >= 1 ⇒ infeasible on both sides."""
        sql = """
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) >= 1 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """
        packdb_cli.assert_error(sql, match=r"(?i)(infeasible|unbounded)")

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 10",
        )
        oracle_solver.create_model("sum_ge_empty_infeas")
        vnames = [f"x_{i}" for i in range(len(data))]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        _per_strict_sum(
            oracle_solver, vnames, data,
            key_fn=lambda r: r[2],
            mask_fn=lambda r: r[2] == "R",
            sense=">=", rhs=1.0, name_prefix="sum_ge",
        )
        oracle_solver.set_objective(
            {v: 1.0 for v in vnames}, ObjSense.MAXIMIZE,
        )
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    def test_sum_le_empty_group_trivially_true(
        self, packdb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM <= 100 with empty group: 0 <= 100 ⇒ trivially satisfied."""
        sql = """
            SELECT l_orderkey, l_linenumber, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) <= 100 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        packdb_rows, packdb_cols = packdb_cli.execute(sql)
        packdb_time = time.perf_counter() - t0

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 10",
        )
        n = len(data)
        t_build = time.perf_counter()
        oracle_solver.create_model("sum_le_empty_trivial")
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        _per_strict_sum(
            oracle_solver, vnames, data,
            key_fn=lambda r: r[2],
            mask_fn=lambda r: r[2] == "R",
            sense="<=", rhs=100.0, name_prefix="sum_le",
        )
        oracle_solver.set_objective(
            {v: 1.0 for v in vnames}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = oracle_solver.solve()

        cmp = compare_solutions(
            packdb_rows, packdb_cols, result, data, ["x"],
            coeff_fn=lambda row: {"x": 1.0},
        )
        perf_tracker.record(
            "sum_le_empty_group_trivially_true", packdb_time, build_time,
            result.solve_time_seconds, n, n, 3,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
        )

    def test_sum_eq_empty_group_infeasible(
        self, packdb_cli, duckdb_conn, oracle_solver
    ):
        """SUM = 3 with empty group: 0 = 3 ⇒ infeasible."""
        sql = """
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) = 3 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """
        packdb_cli.assert_error(sql, match=r"(?i)(infeasible|unbounded)")

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 10",
        )
        oracle_solver.create_model("sum_eq_empty_infeas")
        vnames = [f"x_{i}" for i in range(len(data))]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        _per_strict_sum(
            oracle_solver, vnames, data,
            key_fn=lambda r: r[2],
            mask_fn=lambda r: r[2] == "R",
            sense="=", rhs=3.0, name_prefix="sum_eq",
        )
        oracle_solver.set_objective(
            {v: 1.0 for v in vnames}, ObjSense.MAXIMIZE,
        )
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    def test_per_strict_without_when_same_as_per(
        self, packdb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """Without WHEN, no empty groups are possible — STRICT == non-STRICT."""
        sql_strict = """
            SELECT l_orderkey, l_linenumber, l_returnflag, l_extendedprice, x
            FROM lineitem WHERE l_orderkey < 30
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) <= 5 PER STRICT l_returnflag
            MAXIMIZE SUM(x * l_extendedprice)
        """
        sql_normal = sql_strict.replace("PER STRICT", "PER")

        t0 = time.perf_counter()
        rows_strict, cols_strict = packdb_cli.execute(sql_strict)
        rows_normal, _ = packdb_cli.execute(sql_normal)
        packdb_time = time.perf_counter() - t0

        x_idx = cols_strict.index("x")
        assert sum(int(r[x_idx]) for r in rows_strict) == sum(
            int(r[x_idx]) for r in rows_normal
        ), "STRICT without WHEN should behave identically to PER"

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 30",
        )
        n = len(data)
        t_build = time.perf_counter()
        oracle_solver.create_model("per_strict_no_when")
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        _per_strict_sum(
            oracle_solver, vnames, data,
            key_fn=lambda r: r[2], mask_fn=lambda r: True,
            sense="<=", rhs=5.0, name_prefix="per",
        )
        oracle_solver.set_objective(
            {vnames[i]: data[i][5] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = oracle_solver.solve()

        cmp = compare_solutions(
            rows_strict, cols_strict, result, data, ["x"],
            coeff_fn=lambda row: {"x": float(row[cols_strict.index("l_extendedprice")])},
        )
        perf_tracker.record(
            "per_strict_without_when_same_as_per", packdb_time, build_time,
            result.solve_time_seconds, n, n, 3,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
        )

    def test_convergent_case_feasible(
        self, packdb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM(x*q) <= 50 WHEN R PER STRICT flag — empty groups ⇒ 0 <= 50 ✓."""
        sql = """
            SELECT l_orderkey, l_linenumber, l_returnflag, l_extendedprice,
                   l_quantity, x
            FROM lineitem WHERE l_orderkey < 50
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x * l_quantity) <= 50
                WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x * l_extendedprice)
        """
        t0 = time.perf_counter()
        packdb_rows, packdb_cols = packdb_cli.execute(sql)
        packdb_time = time.perf_counter() - t0

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 50",
        )
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("per_strict_convergent")
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.BINARY)

        groups: dict[object, list[int]] = defaultdict(list)
        for i, row in enumerate(data):
            groups[row[2]].append(i)
        for key, idxs in groups.items():
            qualifying = [i for i in idxs if data[i][2] == "R"]
            coeffs = {vnames[i]: data[i][4] for i in qualifying}
            oracle_solver.add_constraint(
                coeffs, "<=", 50.0, name=f"weighted_sum_{key}",
            )

        oracle_solver.set_objective(
            {vnames[i]: data[i][5] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = oracle_solver.solve()

        cmp = compare_solutions(
            packdb_rows, packdb_cols, result, data, ["x"],
            coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_extendedprice")])},
        )
        perf_tracker.record(
            "per_strict_convergent_case_feasible", packdb_time, build_time,
            result.solve_time_seconds, n, n, len(groups),
            result.objective_value, oracle_solver.solver_name(),
            comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
        )

    def test_multi_column_per_strict(
        self, packdb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """PER STRICT with multi-column grouping."""
        sql = """
            SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
                   l_extendedprice, x
            FROM lineitem WHERE l_orderkey < 30
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) <= 3 PER STRICT (l_returnflag, l_linestatus)
            MAXIMIZE SUM(x * l_extendedprice)
        """
        t0 = time.perf_counter()
        packdb_rows, packdb_cols = packdb_cli.execute(sql)
        packdb_time = time.perf_counter() - t0

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 30",
        )
        n = len(data)

        t_build = time.perf_counter()
        oracle_solver.create_model("per_strict_multi_col")
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.BINARY)
        _per_strict_sum(
            oracle_solver, vnames, data,
            key_fn=lambda r: (r[2], r[3]),
            mask_fn=lambda r: True,
            sense="<=", rhs=3.0, name_prefix="per_multi",
        )
        oracle_solver.set_objective(
            {vnames[i]: data[i][5] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = oracle_solver.solve()

        cmp = compare_solutions(
            packdb_rows, packdb_cols, result, data, ["x"],
            coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_extendedprice")])},
        )
        perf_tracker.record(
            "per_strict_multi_column", packdb_time, build_time,
            result.solve_time_seconds, n, n,
            len(set((r[2], r[3]) for r in data)),
            result.objective_value, oracle_solver.solver_name(),
            comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
        )


# ---------------------------------------------------------------------------
# MIN / MAX + PER STRICT
# ---------------------------------------------------------------------------

@pytest.mark.per_strict
@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.correctness
class TestPerStrictMinMax:

    def test_max_ge_empty_group_infeasible(
        self, packdb_cli, duckdb_conn, oracle_solver
    ):
        """MAX(x) >= 1 hard case on empty group: SUM(y)>=1 with no y ⇒ infeasible."""
        sql = """
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS INTEGER
            SUCH THAT x <= 5
                AND MAX(x) >= 1 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """
        packdb_cli.assert_error(sql, match=r"(?i)(infeasible|unbounded)")

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 10",
        )
        oracle_solver.create_model("max_ge_empty_infeas")
        vnames = [f"x_{i}" for i in range(len(data))]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.INTEGER, lb=0.0, ub=5.0)
        _per_strict_max_hard(
            oracle_solver, vnames, data,
            key_fn=lambda r: r[2],
            mask_fn=lambda r: r[2] == "R",
            rhs=1.0, name_prefix="max_ge",
        )
        oracle_solver.set_objective(
            {v: 1.0 for v in vnames}, ObjSense.MAXIMIZE,
        )
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    def test_min_le_empty_group_infeasible(
        self, packdb_cli, duckdb_conn, oracle_solver
    ):
        """MIN(x) <= 3 hard case on empty group: infeasible."""
        sql = """
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS INTEGER
            SUCH THAT x <= 5
                AND MIN(x) <= 3 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """
        packdb_cli.assert_error(sql, match=r"(?i)(infeasible|unbounded)")

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 10",
        )
        oracle_solver.create_model("min_le_empty_infeas")
        vnames = [f"x_{i}" for i in range(len(data))]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.INTEGER, lb=0.0, ub=5.0)
        _per_strict_min_hard(
            oracle_solver, vnames, data,
            key_fn=lambda r: r[2],
            mask_fn=lambda r: r[2] == "R",
            rhs=3.0, name_prefix="min_le",
        )
        oracle_solver.set_objective(
            {v: 1.0 for v in vnames}, ObjSense.MAXIMIZE,
        )
        assert oracle_solver.solve().status == SolverStatus.INFEASIBLE

    def test_max_le_empty_group_vacuously_true(
        self, packdb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """MAX(x) <= 12 easy case on empty group: no constraints ⇒ vacuous."""
        sql = """
            SELECT l_orderkey, l_linenumber, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS INTEGER
            SUCH THAT x <= 5
                AND MAX(x) <= 12 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        packdb_rows, packdb_cols = packdb_cli.execute(sql)
        packdb_time = time.perf_counter() - t0

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 10",
        )
        n = len(data)
        t_build = time.perf_counter()
        oracle_solver.create_model("max_le_empty_vacuous")
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.INTEGER, lb=0.0, ub=5.0)
        # MAX(x) <= 12 on qualifying rows (R only) is implemented per-row
        # (easy case). Empty groups emit zero constraints.
        for i in range(n):
            if data[i][2] == "R":
                oracle_solver.add_constraint(
                    {vnames[i]: 1.0}, "<=", 12.0, name=f"max_le_{i}",
                )
        oracle_solver.set_objective(
            {v: 1.0 for v in vnames}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = oracle_solver.solve()

        cmp = compare_solutions(
            packdb_rows, packdb_cols, result, data, ["x"],
            coeff_fn=lambda row: {"x": 1.0},
        )
        perf_tracker.record(
            "max_le_empty_group_vacuous", packdb_time, build_time,
            result.solve_time_seconds, n, n, 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
        )

    def test_min_ge_empty_group_vacuously_true(
        self, packdb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """MIN(x) >= 1 easy case on empty group: no constraints ⇒ vacuous."""
        sql = """
            SELECT l_orderkey, l_linenumber, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS INTEGER
            SUCH THAT x <= 5
                AND MIN(x) >= 1 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """
        t0 = time.perf_counter()
        packdb_rows, packdb_cols = packdb_cli.execute(sql)
        packdb_time = time.perf_counter() - t0

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 10",
        )
        n = len(data)
        t_build = time.perf_counter()
        oracle_solver.create_model("min_ge_empty_vacuous")
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.INTEGER, lb=0.0, ub=5.0)
        for i in range(n):
            if data[i][2] == "R":
                oracle_solver.add_constraint(
                    {vnames[i]: 1.0}, ">=", 1.0, name=f"min_ge_{i}",
                )
        oracle_solver.set_objective(
            {v: 1.0 for v in vnames}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = oracle_solver.solve()

        cmp = compare_solutions(
            packdb_rows, packdb_cols, result, data, ["x"],
            coeff_fn=lambda row: {"x": 1.0},
        )
        perf_tracker.record(
            "min_ge_empty_group_vacuous", packdb_time, build_time,
            result.solve_time_seconds, n, n, 0,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
        )


# ---------------------------------------------------------------------------
# <> + PER STRICT
# ---------------------------------------------------------------------------

@pytest.mark.per_strict
@pytest.mark.per_clause
@pytest.mark.cons_comparison
@pytest.mark.correctness
class TestPerStrictNE:

    def test_ne_empty_group_trivially_true(
        self, packdb_cli, duckdb_conn, oracle_solver, perf_tracker
    ):
        """SUM <> 3 empty group: 0 <> 3 ⇒ trivially true."""
        sql = """
            SELECT l_orderkey, l_linenumber, l_returnflag, l_extendedprice, x
            FROM lineitem WHERE l_orderkey < 30
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) <> 3 WHEN l_returnflag = 'R' PER STRICT l_returnflag
                AND SUM(x) <= 15
            MAXIMIZE SUM(x * l_extendedprice)
        """
        t0 = time.perf_counter()
        packdb_rows, packdb_cols = packdb_cli.execute(sql)
        packdb_time = time.perf_counter() - t0

        data = _fetch(
            duckdb_conn,
            f"SELECT {_LINEITEM_COLS} FROM lineitem WHERE l_orderkey < 30",
        )
        n = len(data)
        t_build = time.perf_counter()
        oracle_solver.create_model("per_strict_ne")
        vnames = [f"x_{i}" for i in range(n)]
        for v in vnames:
            oracle_solver.add_variable(v, VarType.BINARY)

        groups: dict[object, list[int]] = defaultdict(list)
        for i, row in enumerate(data):
            groups[row[2]].append(i)
        for key, idxs in groups.items():
            qualifying = [i for i in idxs if data[i][2] == "R"]
            coeffs = {vnames[i]: 1.0 for i in qualifying}
            add_ne_indicator(oracle_solver, coeffs, 3.0, name=f"ne_{key}")

        oracle_solver.add_constraint(
            {v: 1.0 for v in vnames}, "<=", 15.0, name="global_cap",
        )
        oracle_solver.set_objective(
            {vnames[i]: data[i][5] for i in range(n)}, ObjSense.MAXIMIZE,
        )
        build_time = time.perf_counter() - t_build
        result = oracle_solver.solve()

        cmp = compare_solutions(
            packdb_rows, packdb_cols, result, data, ["x"],
            coeff_fn=lambda row: {"x": float(row[packdb_cols.index("l_extendedprice")])},
        )
        perf_tracker.record(
            "per_strict_ne_empty_trivial", packdb_time, build_time,
            result.solve_time_seconds, n, n, len(groups) + 1,
            result.objective_value, oracle_solver.solver_name(),
            comparison_status=cmp.status, decide_vector=cmp.oracle_vector,
        )
