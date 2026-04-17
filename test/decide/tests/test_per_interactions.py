"""Cross-feature PER interaction tests.

PER composed with auxiliary-variable features (Big-M indicators for hard
MIN/MAX constraints, ABS auxiliary variables, multi-variable coefficient
extraction) is the weakest area in the test suite. Each rewrite must
partition its auxiliary state per PER group; a global-vs-grouped scoping
bug produces silently wrong results.

Each test uses oracle compare to detect optimality bugs. A globally-scoped
auxiliary-variable bug would relax the per-group constraints, producing a
better objective for MAXIMIZE / worse for MINIMIZE than the correct value.

Covered:
  - test_per_max_geq_constraint:     PER + hard MAX(>=K) — Big-M per group
  - test_per_min_leq_constraint:     PER + hard MIN(<=K) — Big-M per group
  - test_per_max_eq_constraint:      PER + equality MAX(=K) — easy + hard combined
  - test_per_abs_aggregate:          PER + SUM(ABS(...)) — ABS aux per group
  - test_per_multi_variable:         PER + (BOOLEAN + INTEGER) — multi-var indexing
  - test_when_per_multi_variable:    WHEN + PER + (BOOLEAN + INTEGER) — WHEN mask per group per variable
  - test_count_integer_per:          COUNT(x INTEGER) + PER — Big-M indicators scoped per group
  - test_qp_objective_per_constraint: QP objective + PER constraint — QP path alongside PER
"""

import time

import pytest

from solver.types import VarType, ObjSense, SolverStatus


# ============================================================================
# 1.1a — PER + hard MAX(>=K) constraint
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_per_max_geq_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x * l_quantity) >= 30 PER l_returnflag — Big-M indicators must be
    partitioned per group. A global-scoping bug would let one row's indicator
    satisfy all groups simultaneously."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x * l_quantity) >= 30 PER l_returnflag
        MINIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_returnflag AS VARCHAR)
        FROM lineitem WHERE l_orderkey <= 10
        ORDER BY l_orderkey, l_linenumber
    """).fetchall()
    n = len(data)
    K = 30.0

    t_build = time.perf_counter()
    oracle_solver.create_model("per_max_geq")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames + ynames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Big-M per row: qty_i*x_i - K*y_i >= 0
    # (with M = K, lower bound of x*qty = 0)
    # When y_i=1: qty_i*x_i >= K  → forces x_i=1 AND qty_i >= K
    # When y_i=0: qty_i*x_i >= 0  (trivial)
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {xnames[i]: qty, ynames[i]: -K}, ">=", 0.0, name=f"link_{i}",
        )

    # Per-group: SUM(y_i) >= 1
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[4], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {ynames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"grp_{g}",
        )

    oracle_solver.set_objective(
        {xnames[i]: data[i][3] for i in range(n)}, ObjSense.MINIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_obj = sum(
        int(row[ci["x"]]) * float(row[ci["l_extendedprice"]])
        for row in packdb_result
    )
    assert abs(packdb_obj - result.objective_value) <= 1.0, (
        f"Objective mismatch: PackDB={packdb_obj:.2f}, "
        f"Oracle={result.objective_value:.2f}"
    )

    # Sanity: each group has at least one row with x=1 AND qty>=30
    by_grp: dict[str, list[tuple[int, float]]] = {}
    for row in packdb_result:
        flag = str(row[ci["l_returnflag"]])
        by_grp.setdefault(flag, []).append(
            (int(row[ci["x"]]), float(row[ci["l_quantity"]]))
        )
    for g, rows in by_grp.items():
        assert any(x == 1 and qty >= K for x, qty in rows), (
            f"Group {g} has no x=1 row with qty>={K} — per-group MAX>=K violated"
        )

    perf_tracker.record(
        "per_max_geq", packdb_time, build_time,
        result.solve_time_seconds, n, n * 2, n + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.1b — PER + hard MIN(<=K) constraint
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_min_leq_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MIN(x * l_quantity) <= 20 PER l_returnflag with INTEGER x in [1,5].
    The lower bound x>=1 prevents the trivial x=0 escape, so the MIN constraint
    actively limits at least one row's x value per group. Big-M indicators must
    be per-group."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x IS INTEGER
        SUCH THAT x >= 1 AND x <= 5
            AND MIN(x * l_quantity) <= 20 PER l_returnflag
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_returnflag AS VARCHAR)
        FROM lineitem WHERE l_orderkey <= 10
        ORDER BY l_orderkey, l_linenumber
    """).fetchall()
    n = len(data)
    K = 20.0
    X_LB, X_UB = 1, 5
    qty_max = max(d[2] for d in data)
    M = X_UB * qty_max - K  # so when y=0, x*qty <= K + M = X_UB*qty_max ✓

    t_build = time.perf_counter()
    oracle_solver.create_model("per_min_leq")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=X_LB, ub=X_UB)
    for vn in ynames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Big-M per row: qty_i*x_i + M*y_i <= K + M   (rewrite of x*qty <= K + M*(1-y))
    # When y_i=1: qty_i*x_i <= K
    # When y_i=0: qty_i*x_i <= K + M = X_UB*qty_max  (trivial upper bound)
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {xnames[i]: qty, ynames[i]: M}, "<=", K + M, name=f"link_{i}",
        )

    # Per-group: SUM(y_i) >= 1
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[4], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {ynames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"grp_{g}",
        )

    oracle_solver.set_objective(
        {xnames[i]: data[i][3] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_obj = sum(
        int(row[ci["x"]]) * float(row[ci["l_extendedprice"]])
        for row in packdb_result
    )
    assert abs(packdb_obj - result.objective_value) <= 1.0, (
        f"Objective mismatch: PackDB={packdb_obj:.2f}, "
        f"Oracle={result.objective_value:.2f}"
    )

    # Sanity: each group has at least one row with x*qty <= K
    by_grp: dict[str, list[float]] = {}
    for row in packdb_result:
        flag = str(row[ci["l_returnflag"]])
        x_val = int(row[ci["x"]])
        qty = float(row[ci["l_quantity"]])
        by_grp.setdefault(flag, []).append(x_val * qty)
    for g, vals in by_grp.items():
        assert min(vals) <= K + 1e-6, (
            f"Group {g} MIN(x*qty)={min(vals)} > {K} — per-group MIN<=K violated"
        )

    perf_tracker.record(
        "per_min_leq", packdb_time, build_time,
        result.solve_time_seconds, n, n * 2, n + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.1c — PER + equality MAX(=K) constraint (easy + hard combined)
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_max_eq_constraint(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """MAX(x * l_quantity) = 30 PER l_returnflag with INTEGER x in [0,5].
    Combines easy direction (per-row x*qty <= 30) with hard direction
    (per-group existence of x*qty = 30). Both must be partitioned per group
    for the equality semantics to hold per group. Uses l_orderkey <= 100 to
    ensure each returnflag group contains at least one (qty,x) factorization
    of 30."""
    sql = """
        SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 100
        DECIDE x IS INTEGER
        SUCH THAT x >= 0 AND x <= 5
            AND MAX(x * l_quantity) = 30 PER l_returnflag
        MAXIMIZE SUM(x * l_extendedprice)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = duckdb_conn.execute("""
        SELECT CAST(l_orderkey AS BIGINT),
               CAST(l_linenumber AS BIGINT),
               CAST(l_quantity AS DOUBLE),
               CAST(l_extendedprice AS DOUBLE),
               CAST(l_returnflag AS VARCHAR)
        FROM lineitem WHERE l_orderkey <= 100
        ORDER BY l_orderkey, l_linenumber
    """).fetchall()
    n = len(data)
    K = 30.0
    X_LB, X_UB = 0, 5

    t_build = time.perf_counter()
    oracle_solver.create_model("per_max_eq")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=X_LB, ub=X_UB)
    for vn in ynames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Easy direction (per-row): qty_i * x_i <= K
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {xnames[i]: qty}, "<=", K, name=f"easy_{i}",
        )

    # Hard direction (per group): exists i with qty_i*x_i >= K
    # Big-M per row: qty_i*x_i - K*y_i >= 0  (M = K, since x>=0 means x*qty>=0)
    for i in range(n):
        qty = data[i][2]
        oracle_solver.add_constraint(
            {xnames[i]: qty, ynames[i]: -K}, ">=", 0.0, name=f"hard_{i}",
        )

    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[4], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {ynames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"grp_{g}",
        )

    oracle_solver.set_objective(
        {xnames[i]: data[i][3] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_obj = sum(
        int(row[ci["x"]]) * float(row[ci["l_extendedprice"]])
        for row in packdb_result
    )
    assert abs(packdb_obj - result.objective_value) <= 1.0, (
        f"Objective mismatch: PackDB={packdb_obj:.2f}, "
        f"Oracle={result.objective_value:.2f}"
    )

    # Sanity: each group has MAX(x*qty) == K
    by_grp: dict[str, list[float]] = {}
    for row in packdb_result:
        flag = str(row[ci["l_returnflag"]])
        x_val = int(row[ci["x"]])
        qty = float(row[ci["l_quantity"]])
        by_grp.setdefault(flag, []).append(x_val * qty)
    for g, vals in by_grp.items():
        assert abs(max(vals) - K) <= 1e-6, (
            f"Group {g} MAX(x*qty)={max(vals)} != {K} — per-group MAX=K violated"
        )

    perf_tracker.record(
        "per_max_eq", packdb_time, build_time,
        result.solve_time_seconds, n, n * 2, n * 2 + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.2 — PER + ABS in aggregate constraint
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_abs_aggregate(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """SUM(ABS(x - target)) <= 5 PER grp — ABS auxiliary variables must be
    partitioned by PER group. A global-scoping bug would let cross-group
    deviations cancel against each other in the aggregate, relaxing the
    per-group bound."""
    sql = """
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
            SELECT 2, 'A', 15.0 UNION ALL
            SELECT 3, 'B', 20.0 UNION ALL
            SELECT 4, 'B', 25.0
        )
        SELECT id, grp, target, x
        FROM data
        DECIDE x IS REAL
        SUCH THAT x <= 50
            AND SUM(ABS(x - target)) <= 5 PER grp
        MAXIMIZE SUM(x)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    # Mirror the inline CTE for the oracle.
    data = [
        (1, 'A', 10.0),
        (2, 'A', 15.0),
        (3, 'B', 20.0),
        (4, 'B', 25.0),
    ]
    n = len(data)
    K = 5.0

    t_build = time.perf_counter()
    oracle_solver.create_model("per_abs_aggregate")
    xnames = [f"x_{i}" for i in range(n)]
    dnames = [f"d_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0, ub=50.0)
    for vn in dnames:
        oracle_solver.add_variable(vn, VarType.CONTINUOUS, lb=0.0)

    # ABS linearization: d_i >= x_i - target_i,  d_i >= -(x_i - target_i)
    for i in range(n):
        target = data[i][2]
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: -1.0}, ">=", -target, name=f"abs_pos_{i}",
        )
        oracle_solver.add_constraint(
            {dnames[i]: 1.0, xnames[i]: 1.0}, ">=", target, name=f"abs_neg_{i}",
        )

    # Per-group: SUM(d_i) <= K
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {dnames[i]: 1.0 for i in idxs}, "<=", K, name=f"grp_{g}",
        )

    oracle_solver.set_objective(
        {xnames[i]: 1.0 for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_obj = sum(float(row[ci["x"]]) for row in packdb_result)
    assert abs(packdb_obj - result.objective_value) <= 1e-4, (
        f"Objective mismatch: PackDB={packdb_obj:.6f}, "
        f"Oracle={result.objective_value:.6f}"
    )

    # Sanity: per-group SUM(|x - target|) <= K
    by_grp: dict[str, float] = {}
    for row in packdb_result:
        g = str(row[ci["grp"]])
        x_val = float(row[ci["x"]])
        target = float(row[ci["target"]])
        by_grp[g] = by_grp.get(g, 0.0) + abs(x_val - target)
    for g, total in by_grp.items():
        assert total <= K + 1e-4, (
            f"Group {g} SUM|x-target|={total:.6f} > {K} — per-group constraint violated"
        )

    perf_tracker.record(
        "per_abs_aggregate", packdb_time, build_time,
        result.solve_time_seconds, n, n * 2, n * 2 + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.3 — Multi-variable + PER
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.var_multi
@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_per_multi_variable(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Two decision variables (BOOLEAN x, INTEGER y) under PER grouping.
    The variable-indexing layer must produce the right column mapping for
    each group's constraint, with each variable's coefficients partitioned
    correctly. A multi-variable indexing bug under PER could swap or drop
    coefficients silently."""
    sql = """
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, 10 AS w UNION ALL
            SELECT 2, 'A', 5 UNION ALL
            SELECT 3, 'B', 8 UNION ALL
            SELECT 4, 'B', 3
        )
        SELECT id, grp, w, x, y
        FROM data
        DECIDE x IS BOOLEAN, y IS INTEGER
        SUCH THAT SUM(x * w) <= 12 PER grp
            AND y <= 3
            AND SUM(y) <= 8
        MAXIMIZE SUM(x * w + y)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    data = [
        (1, 'A', 10),
        (2, 'A', 5),
        (3, 'B', 8),
        (4, 'B', 3),
    ]
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("per_multi_var")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in ynames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0, ub=3)

    # Per-group: SUM(x_i * w_i) <= 12
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {xnames[i]: data[i][2] for i in idxs}, "<=", 12.0, name=f"grp_{g}",
        )

    # Global: SUM(y_i) <= 8
    oracle_solver.add_constraint(
        {ynames[i]: 1.0 for i in range(n)}, "<=", 8.0, name="sum_y",
    )

    # MAXIMIZE SUM(x_i * w_i + y_i)
    obj: dict[str, float] = {}
    for i in range(n):
        obj[xnames[i]] = float(data[i][2])
        obj[ynames[i]] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: i for i, name in enumerate(packdb_cols)}
    packdb_obj = sum(
        int(row[ci["x"]]) * int(row[ci["w"]]) + int(row[ci["y"]])
        for row in packdb_result
    )
    assert abs(packdb_obj - result.objective_value) <= 0.5, (
        f"Objective mismatch: PackDB={packdb_obj}, "
        f"Oracle={result.objective_value:.0f}"
    )

    # Sanity: per-group SUM(x*w) <= 12 and global SUM(y) <= 8
    by_grp: dict[str, float] = {}
    sum_y = 0
    for row in packdb_result:
        g = str(row[ci["grp"]])
        xw = int(row[ci["x"]]) * int(row[ci["w"]])
        by_grp[g] = by_grp.get(g, 0.0) + xw
        sum_y += int(row[ci["y"]])
    for g, total in by_grp.items():
        assert total <= 12, f"Group {g} SUM(x*w)={total} > 12 — PER violated"
    assert sum_y <= 8, f"SUM(y)={sum_y} > 8 — global constraint violated"

    perf_tracker.record(
        "per_multi_var", packdb_time, build_time,
        result.solve_time_seconds, n, n * 2, len(groups) + 1,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.4 — WHEN + PER + multi-variable
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.var_multi
@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_when_per_multi_variable(packdb_cli, oracle_solver, perf_tracker):
    """WHEN + PER + two decision variables (BOOLEAN x, INTEGER y).

    The WHEN filter applies to the per-group aggregate constraint — only
    active rows contribute to each group's sum. The objective includes all
    rows (no WHEN). A shared coefficient accumulator bug would mix WHEN-masked
    rows of one variable with unmasked rows of the other, silently corrupting
    the per-group aggregate.

    Data layout (row 2 is inactive in group A):
      id=1 grp=A active=T  w=10 v=5  → in WHEN constraint for A
      id=2 grp=A active=F  w=6  v=3  → only in objective (no constraint from WHEN)
      id=3 grp=B active=T  w=8  v=4  → in WHEN constraint for B
      id=4 grp=B active=T  w=7  v=2  → in WHEN constraint for B
    """
    sql = """
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, true  AS active, 10 AS w, 5 AS v UNION ALL
            SELECT 2, 'A', false, 6, 3 UNION ALL
            SELECT 3, 'B', true,  8, 4 UNION ALL
            SELECT 4, 'B', true,  7, 2
        )
        SELECT id, grp, w, v, x, y FROM data
        DECIDE x IS BOOLEAN, y IS INTEGER
        SUCH THAT y <= 5 AND SUM(x * w + y * v) <= 18 WHEN active PER grp
        MAXIMIZE SUM(x * w + y * v)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    # Inline CTE data: (id, grp, active, w, v)
    data = [
        (1, 'A', True,  10, 5),
        (2, 'A', False,  6, 3),
        (3, 'B', True,   8, 4),
        (4, 'B', True,   7, 2),
    ]
    n = len(data)

    t_build = time.perf_counter()
    oracle_solver.create_model("when_per_multi_var")
    xnames = [f"x_{i}" for i in range(n)]
    ynames = [f"y_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.BINARY)
    for vn in ynames:
        # y <= 5 captured by ub=5; default lb=0 from add_variable
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0, ub=5)

    # Per-group WHEN-filtered aggregate: SUM_{active rows in g} (w_i*x_i + v_i*y_i) <= 18
    groups_active: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        if row[2]:  # active
            groups_active.setdefault(row[1], []).append(i)
    for g, idxs in groups_active.items():
        coeffs: dict[str, float] = {}
        for i in idxs:
            coeffs[xnames[i]] = float(data[i][3])  # w_i
            coeffs[ynames[i]] = float(data[i][4])  # v_i
        oracle_solver.add_constraint(coeffs, "<=", 18.0, name=f"when_per_{g}")

    # Objective: ALL rows (no WHEN filter)
    obj: dict[str, float] = {}
    for i in range(n):
        obj[xnames[i]] = float(data[i][3])  # w_i
        obj[ynames[i]] = float(data[i][4])  # v_i
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: j for j, name in enumerate(packdb_cols)}
    packdb_obj = sum(
        int(row[ci["x"]]) * int(row[ci["w"]]) + int(row[ci["y"]]) * int(row[ci["v"]])
        for row in packdb_result
    )
    assert abs(packdb_obj - result.objective_value) <= 0.5, (
        f"Objective mismatch: PackDB={packdb_obj}, "
        f"Oracle={result.objective_value:.0f}"
    )

    # Sanity: per-group WHEN-filtered sum <= 18
    row_by_id = {int(row[ci["id"]]): row for row in packdb_result}
    by_grp_active: dict[str, float] = {}
    for row_id, grp, active, w, v in data:
        if active:
            row = row_by_id[row_id]
            contrib = int(row[ci["x"]]) * w + int(row[ci["y"]]) * v
            by_grp_active[grp] = by_grp_active.get(grp, 0.0) + contrib
    for g, total in by_grp_active.items():
        assert total <= 18 + 1e-9, (
            f"Group {g} WHEN-filtered SUM(x*w+y*v)={total} > 18 — constraint violated"
        )

    perf_tracker.record(
        "when_per_multi_var", packdb_time, build_time,
        result.solve_time_seconds, n, n * 2, len(groups_active),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.5 — COUNT(x INTEGER) + PER
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.count_rewrite
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_count_integer_per(packdb_cli, oracle_solver, perf_tracker):
    """COUNT(x INTEGER) >= 1 PER grp — Big-M indicator variables must be
    scoped per group.

    PackDB rewrites COUNT(x) to a binary indicator z_i with constraints
    z_i <= x_i and x_i <= M*z_i, then enforces SUM(z_i) >= 1 per group.
    A global-scoping bug would allow a non-zero x in group A to satisfy the
    COUNT requirement in group B, yielding an overly relaxed problem.

    The global budget SUM(x) <= 10 is the key stress: without it, x=10
    everywhere makes every z=1, so scoping doesn't matter. With the budget,
    the optimizer wants to concentrate budget in group A (higher val). If the
    COUNT per-group scoping is wrong, it can zero out group B entirely and
    score 100 (x_A=10). The correctly scoped oracle is forced to keep at least
    one non-zero x in group B, yielding obj=98 (x_A=9, x_B=1 with val_B=8).

    Data: 2 rows in group A (val=10,5), 2 rows in group B (val=8,3).
    """
    sql = """
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, 10 AS val UNION ALL
            SELECT 2, 'A', 5 UNION ALL
            SELECT 3, 'B', 8 UNION ALL
            SELECT 4, 'B', 3
        )
        SELECT id, grp, val, x FROM data
        DECIDE x IS INTEGER
        SUCH THAT x <= 10 AND SUM(x) <= 10 AND COUNT(x) >= 1 PER grp
        MAXIMIZE SUM(x * val)
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    # Inline CTE data: (id, grp, val)
    data = [
        (1, 'A', 10),
        (2, 'A',  5),
        (3, 'B',  8),
        (4, 'B',  3),
    ]
    n = len(data)
    M = 10  # upper bound on x

    t_build = time.perf_counter()
    oracle_solver.create_model("count_integer_per")
    xnames = [f"x_{i}" for i in range(n)]
    znames = [f"z_{i}" for i in range(n)]
    for vn in xnames:
        oracle_solver.add_variable(vn, VarType.INTEGER, lb=0, ub=M)
    for vn in znames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    # Big-M linking: z_i <= x_i  →  {z_i: 1, x_i: -1} <= 0
    for i in range(n):
        oracle_solver.add_constraint(
            {znames[i]: 1.0, xnames[i]: -1.0}, "<=", 0.0, name=f"bm_lo_{i}",
        )
    # Big-M linking: x_i <= M * z_i  →  {x_i: 1, z_i: -M} <= 0
    for i in range(n):
        oracle_solver.add_constraint(
            {xnames[i]: 1.0, znames[i]: -float(M)}, "<=", 0.0, name=f"bm_hi_{i}",
        )

    # Global budget: SUM(x_i) <= 10
    oracle_solver.add_constraint(
        {xnames[i]: 1.0 for i in range(n)}, "<=", 10.0, name="budget",
    )

    # Per-group COUNT >= 1: SUM_{i in g} z_i >= 1
    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {znames[i]: 1.0 for i in idxs}, ">=", 1.0, name=f"count_per_{g}",
        )

    oracle_solver.set_objective(
        {xnames[i]: float(data[i][2]) for i in range(n)}, ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    assert result.status == SolverStatus.OPTIMAL

    ci = {name: j for j, name in enumerate(packdb_cols)}
    packdb_obj = sum(
        int(row[ci["x"]]) * int(row[ci["val"]]) for row in packdb_result
    )
    assert abs(packdb_obj - result.objective_value) <= 0.5, (
        f"Objective mismatch: PackDB={packdb_obj}, "
        f"Oracle={result.objective_value:.0f} — "
        f"if PackDB ~100 vs oracle ~98, COUNT scoping is global not per-group"
    )

    # Sanity: each group has at least one non-zero x (COUNT >= 1 per group)
    by_grp: dict[str, list[int]] = {}
    for row in packdb_result:
        g = str(row[ci["grp"]])
        by_grp.setdefault(g, []).append(int(row[ci["x"]]))
    for g, vals in by_grp.items():
        assert any(v > 0 for v in vals), (
            f"Group {g} has no non-zero x — COUNT(x)>=1 per group violated"
        )

    # Sanity: global budget respected
    total_x = sum(int(row[ci["x"]]) for row in packdb_result)
    assert total_x <= 10, f"Global SUM(x)={total_x} > 10 — budget violated"

    perf_tracker.record(
        "count_integer_per", packdb_time, build_time,
        result.solve_time_seconds, n, n * 2, n * 2 + 1 + len(groups),
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )


# ============================================================================
# 1.6 — QP objective + PER constraint
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.quadratic
@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_qp_objective_per_constraint(
    packdb_cli, oracle_solver, perf_tracker,
):
    """QP objective (MINIMIZE SUM(POWER(x - target, 2))) with a linear PER
    constraint (SUM(x) >= 5 PER grp).

    Oracle-compared via ``set_quadratic_objective`` so both the QP path and
    the PER path are exercised together; a silently-dropped PER constraint
    would produce obj≈0 instead of matching the oracle's ~16.
    """
    sql = """
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, 0.5 AS target UNION ALL
            SELECT 2, 'A', 0.5 UNION ALL
            SELECT 3, 'B', 0.5 UNION ALL
            SELECT 4, 'B', 0.5
        )
        SELECT id, grp, target, ROUND(x::DOUBLE, 6) AS x
        FROM data
        DECIDE x IS REAL
        SUCH THAT x >= 0 AND x <= 100 AND SUM(x) >= 5 PER grp
        MINIMIZE SUM(POWER(x - target, 2))
    """
    t0 = time.perf_counter()
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    packdb_time = time.perf_counter() - t0

    # Oracle: mirror the SQL with row-indexed continuous vars, a PER-group
    # SUM(x) >= 5 constraint, and the expanded POWER quadratic. Expansion of
    # (x_i - t_i)^2 drops the constant t_i^2 term (PackDB's reported
    # objective does the same), leaving linear = -2*t_i * x_i and
    # quadratic = x_i^2.
    data = [
        (1, "A", 0.5), (2, "A", 0.5), (3, "B", 0.5), (4, "B", 0.5),
    ]
    n = len(data)
    t_build = time.perf_counter()
    oracle_solver.create_model("qp_objective_per_constraint")
    xnames = [f"x_{i}" for i in range(n)]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.CONTINUOUS, lb=0.0, ub=100.0)

    groups: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        groups.setdefault(row[1], []).append(i)
    for g, idxs in groups.items():
        oracle_solver.add_constraint(
            {xnames[i]: 1.0 for i in idxs}, ">=", 5.0, name=f"per_{g}",
        )

    linear = {xnames[i]: -2.0 * float(data[i][2]) for i in range(n)}
    quadratic = {(xnames[i], xnames[i]): 1.0 for i in range(n)}
    oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE)
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    ci = {name: j for j, name in enumerate(packdb_cols)}
    # Packdb-side objective: strip the constant t_i^2 term to match the
    # oracle's constant-free formulation.
    packdb_obj = sum(
        (float(row[ci["x"]]) - float(row[ci["target"]])) ** 2
        - float(row[ci["target"]]) ** 2
        for row in packdb_result
    )
    assert abs(packdb_obj - result.objective_value) <= 1e-3, (
        f"QP+PER objective mismatch: PackDB={packdb_obj:.6f}, "
        f"Oracle={result.objective_value:.6f}"
    )

    # Invariant: PER constraint must hold per group.
    by_grp: dict[str, float] = {}
    for row in packdb_result:
        g = str(row[ci["grp"]])
        by_grp[g] = by_grp.get(g, 0.0) + float(row[ci["x"]])
    for g, total in by_grp.items():
        assert total >= 5 - 1e-6, (
            f"Group {g} SUM(x)={total:.4f} < 5 — PER constraint violated"
        )

    perf_tracker.record(
        "qp_objective_per", packdb_time, build_time,
        result.solve_time_seconds, n, n, 2 + n,
        result.objective_value, oracle_solver.solver_name(),
        comparison_status="optimal",
    )
