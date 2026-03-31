# Oracle-Based DECIDE Testing Framework

Pytest-based differential testing framework that validates PackDB's DECIDE clause
by comparing its output against hand-written ILP models solved by an independent
oracle (HiGHS or Gurobi).

Located at `test/decide/`.

---

## 1. How It Works

```
                       ┌──────────────┐
  DECIDE SQL ─────────►│   PackDB     │──► rows + objective
                       └──────────────┘
                              │
                              ▼
                       ┌──────────────┐
  same data ─────────►│ Oracle Solver │──► objective value
  (fetched via SQL)    └──────────────┘
                              │
                              ▼
                    |packdb_obj - oracle_obj| <= ε
```

Each test:
1. Runs a DECIDE query through PackDB and captures output rows.
2. Fetches the same underlying data via plain SQL.
3. Builds an equivalent ILP model in Python (by hand, no SQL parsing).
4. Solves the model with HiGHS (or Gurobi if available).
5. Computes PackDB's achieved objective from its output rows.
6. Asserts the two objective values match within tolerance (default 1e-4).

Only objective values are compared — not variable assignments — because ILP
problems frequently have multiple optimal solutions.

---

## 2. Directory Structure

```
test/decide/
├── run_tests.sh               # Virtualenv manager + test runner
├── conftest.py                # Fixtures, markers, session hooks
├── pytest.ini                 # Marker registration
├── requirements.txt           # highspy, pytest
├── .gitignore
│
├── solver/                    # Solver abstraction layer
│   ├── types.py               #   SolverResult, VarType, ObjSense enums
│   ├── base.py                #   OracleSolver ABC
│   ├── highs_backend.py       #   HiGHS wrapper (primary)
│   ├── gurobi_backend.py      #   Gurobi wrapper (optional)
│   └── factory.py             #   Auto-detect best available solver
│
├── comparison/
│   └── compare.py             # assert_optimal_match, assert_infeasible
│
├── performance/
│   ├── tracker.py             # PerfTracker / PerfRecord
│   └── reporter.py            # CLI table printer, JSON output
│
├── results/                   # JSON perf files (gitignored)
└── tests/                     # All test files
```

---

## 3. Infrastructure Components

### 3.1 Solver Abstraction (`solver/`)

**`types.py`** — Shared enums and result type:
- `VarType`: BINARY, INTEGER, CONTINUOUS
- `ObjSense`: MAXIMIZE, MINIMIZE
- `SolverStatus`: OPTIMAL, INFEASIBLE, UNBOUNDED, etc.
- `SolverResult`: status, objective_value, variable_values dict, solve_time_seconds

**`base.py`** — `OracleSolver` abstract base class:
```python
create_model(name)              # Start a new model
add_variable(name, var_type)    # Add decision variable
add_constraint(coeffs, sense, rhs)  # coeffs = {var_name: coeff}
set_objective(coeffs, sense)    # Set linear objective
solve(time_limit=60.0)          # Returns SolverResult
solver_name()                   # "HiGHS" or "Gurobi"
```

**`highs_backend.py`** — Wraps `highspy.Highs()`. Uses `addCol`/`addRow`/
`changeColCost`/`changeObjectiveSense`. Maps HiGHS status codes to `SolverStatus`.

**`gurobi_backend.py`** — Wraps `gurobipy.Model`. Uses `addVar`/`addConstr`/
`LinExpr`/`setObjective`. Maps Gurobi `model.status` to `SolverStatus`.

**`factory.py`** — `get_solver()` tries `import gurobipy` first, falls back to
`import highspy`.

### 3.2 Comparison (`comparison/compare.py`)

**`assert_optimal_match(packdb_rows, packdb_cols, oracle_result, decide_var_names, coeff_fn, tolerance=1e-4)`**

Computes PackDB's objective by iterating output rows:
```
packdb_obj = Σ (row[var] × coeff_fn(row)[var])  for each row, each var
```

The `coeff_fn` parameter is a callable that, given a PackDB output row, returns a
dict `{var_name: coefficient}`. This lets each test specify its own coefficient
extraction logic, which is critical for complex expressions like
`x * price * (1 - discount) * (1 + tax)`.

### 3.3 Fixtures (`conftest.py`)

| Fixture | Scope | Purpose |
|---------|-------|---------|
| `packdb_db_path` | session | Path to `packdb.db` (TPC-H SF-0.01); skip if missing |
| `packdb_conn` | function | In-memory packdb connection with TPC-H attached read-only |
| `duckdb_conn` | function | Second packdb connection for data fetching (see caveat 6) |
| `oracle_solver` | session | Auto-detected solver instance |
| `perf_tracker` | session | Collects timing; writes JSON + prints table on teardown |

### 3.4 Performance Tracking (`performance/`)

Each correctness test calls `perf_tracker.record(...)` with:
- PackDB wall-clock time, oracle build time, oracle solve time
- Problem dimensions (rows, variables, constraints)
- Objective value and solver backend name

On session teardown, the tracker saves a timestamped JSON file to `results/`
and prints a CLI summary table.

---

## 4. Running Tests

```bash
cd test/decide
bash run_tests.sh                          # All tests
bash run_tests.sh -k "test_q01"            # Single test
bash run_tests.sh -m "var_boolean"         # By marker
bash run_tests.sh -m "correctness"         # All oracle comparisons
bash run_tests.sh -m "error"              # All error tests
bash run_tests.sh -m "not large_scale"    # Skip perf tests
```

`run_tests.sh` creates a `.venv/` virtualenv on first run, installs deps
(`highspy`, `pytest`, packdb from `tools/pythonpkg`), and invokes pytest.

---

## 5. Test Categories & Markers

| Marker | Description |
|--------|-------------|
| `var_boolean` | IS BOOLEAN decision variables |
| `var_integer` | IS INTEGER / default type |
| `cons_aggregate` | SUM-based aggregate constraints |
| `cons_perrow` | Per-row bounds (`x <= 5`, `x <= col`) |
| `cons_mixed` | Aggregate + per-row combined |
| `cons_between` | BETWEEN ... AND ... |
| `cons_subquery` | Subquery on constraint RHS |
| `cons_multi` | Multiple constraints |
| `obj_maximize` | MAXIMIZE objective |
| `obj_minimize` | MINIMIZE objective |
| `obj_complex` | Complex coefficient arithmetic |
| `sql_joins` | JOINs with DECIDE |
| `sql_subquery` | SQL subquery features |
| `error_parser` | Parser-level syntax errors |
| `error_binder` | Binder-level semantic errors |
| `error_infeasible` | Infeasible models |
| `per_clause` | PER keyword |
| `large_scale` | Performance / scale tests |
| `correctness` | Meta: all oracle comparison tests |
| `error` | Meta: all error tests |
| `performance` | Meta: all performance tests |

---

## 6. Correctness Test Reference

### 6.1 Variable Type Tests (`test_var_boolean.py`, `test_var_integer.py`)

**test_q01_knapsack_binary** — Classic 0/1 knapsack on lineitem.
```sql
SELECT ... FROM lineitem WHERE l_orderkey < 100
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_quantity) <= 100
MAXIMIZE SUM(x * l_extendedprice)
```
Oracle: binary variables, one capacity constraint, maximize total price.

**test_knapsack_lineitem** — Variant with weight limit 500 on l_orderkey <= 200.

**test_simple_test** — Default (INTEGER) variable type, bounded 0-10.
```sql
SELECT ... FROM lineitem WHERE l_orderkey <= 10
DECIDE x
SUCH THAT SUM(x * l_quantity) <= 500 AND x <= 10
MAXIMIZE SUM(x * l_extendedprice)
```
Oracle: integer variables with upper bound 10, one aggregate constraint.

### 6.2 Constraint Tests

**test_q08_marketing_campaign** (`test_cons_aggregate.py`) — Boolean selection
with budget constraint: `SUM(x * c_acctbal) <= 5000`.

**test_order_selection** (`test_cons_aggregate.py`) — Select orders under weight
limit: `SUM(x * o_totalprice) <= 50000`.

**test_q07_row_wise_bounds** (`test_cons_perrow.py`) — Per-row + aggregate:
```sql
DECIDE x IS INTEGER
SUCH THAT x <= 5 AND SUM(x * ps_supplycost) <= 1000
MAXIMIZE SUM(x * ps_availqty)
```
Oracle: integer vars with `ub=5`, aggregate budget constraint.

**test_q02_integer_procurement** (`test_cons_mixed.py`) — Per-row + aggregate:
```sql
DECIDE x IS INTEGER
SUCH THAT x <= ps_availqty AND SUM(x * ps_supplycost) <= 5000
MAXIMIZE SUM(x * ps_availqty)
```
Oracle: upper bound per variable from `ps_availqty` column.

**test_q10_logic_dependency** (`test_cons_between.py`) — BETWEEN + multi-constraint:
```sql
DECIDE x IS INTEGER
SUCH THAT x BETWEEN 0 AND 5
  AND SUM(x) <= 100
  AND SUM(x * l_extendedprice) <= 100000
MAXIMIZE SUM(x * l_extendedprice * (1 - l_discount))
```
Oracle: variables bounded [0, 5], two aggregate constraints.

**test_q04_subquery_rhs** (`test_cons_subquery.py`) — Subquery as constraint bound:
```sql
SUCH THAT SUM(x * o_totalprice) <= (SELECT AVG(c_acctbal) FROM tpch.customer WHERE c_nationkey = 10)
```
Oracle: resolves the subquery via plain SQL first, then uses the scalar value.

**test_q06_multi_constraint** (`test_cons_multi.py`) — Two constraints with join:
```sql
FROM lineitem, orders WHERE l_orderkey = o_orderkey AND l_orderkey <= 50
SUCH THAT SUM(x * l_quantity) <= 200 AND SUM(x * o_totalprice) <= 50000
```
Oracle: two aggregate constraints on different columns from joined data.

### 6.3 Objective Tests

**test_q09_minimize_cost** (`test_obj_minimize.py`) — Minimize selection:
```sql
FROM supplier WHERE s_nationkey <= 5
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 10
MINIMIZE SUM(x * s_acctbal)
```
Oracle: select at least 10, minimize total acctbal.

**test_min_cost_supplier** (`test_obj_minimize.py`) — Minimize cost to meet demand:
```sql
SUCH THAT SUM(x * ps_availqty) >= 1000
MINIMIZE SUM(x * ps_supplycost)
```

**test_q03_complex_coeffs** (`test_obj_complex_coeffs.py`) — Multi-column arithmetic:
```sql
MAXIMIZE SUM(x * l_extendedprice * (1 - l_discount) * (1 + l_tax))
```
Oracle: pre-computes `price * (1 - discount) * (1 + tax)` per row.

### 6.4 SQL Feature Tests

**test_q05_join_decide** (`test_sql_joins.py`) — JOIN in FROM clause:
```sql
FROM orders, customer
WHERE o_custkey = c_custkey AND c_nationkey = 10
```
Oracle: fetches joined result set, builds model on joined rows.

### 6.5 Scale Tests (`test_large_scale.py`)

**test_knapsack_large** — 501 rows (lineitem, orderkey <= 100).
**test_order_selection_large** — 2204 rows (orders, orderkey <= 3000).

Both compare objective values and record timing.

---

## 7. Error Test Reference

### 7.1 Parser Errors (`test_error_parser.py`) — 4 tests

| Test | What it checks |
|------|----------------|
| `test_missing_such_that` | DECIDE without SUCH THAT |
| `test_missing_variable_name` | DECIDE IS BOOLEAN (no name) |
| `test_missing_decide_keyword` | SUCH THAT without DECIDE |
| `test_empty_constraint` | SUCH THAT (empty) |

All expect `packdb.ParserException`.

### 7.2 Binder Errors (`test_error_binder.py`) — 16 tests

| Test | Error |
|------|-------|
| `test_variable_conflicts_with_column` | Name clashes with existing column |
| `test_duplicate_decide_variables` | Same variable declared twice |
| `test_unknown_variable_in_constraint` | Undeclared var in IN list |
| `test_is_null_unsupported` | IS NULL in SUCH THAT |
| `test_sum_with_in_not_allowed` | SUM(x) IN (...) |
| `test_non_decide_variable_in_constraint` | Regular column constrained |
| `test_non_sum_function_in_constraint` | AVG(x) instead of SUM |
| `test_no_decide_variable_in_sum` | SUM(col) without decide var |
| `test_multiple_decide_variables_in_sum` | x*x is quadratic |
| `test_nonlinear_decide_variables` | Non-linearity caught at execution |
| `test_between_non_scalar` | SUM BETWEEN with non-scalar bound |
| `test_decide_between_decide_variable` | DECIDE var in BETWEEN bounds |
| `test_in_rhs_with_decide_variable` | DECIDE var in IN list values |
| `test_sum_rhs_non_scalar` | SUM compared to non-scalar |
| `test_decide_variable_rhs_with_decide` | Var compared to var |
| `test_sum_equal_non_scalar` | SUM = non-scalar |
| `test_objective_with_addition` | SUM(...)+3 in objective |
| `test_objective_bare_column` | Bare column as objective |
| `test_subquery_rhs_non_scalar` | Subquery referencing DECIDE var |

### 7.3 Infeasibility Tests (`test_error_infeasible.py`) — 3 tests

| Test | Contradiction |
|------|---------------|
| `test_contradictory_bounds` | x >= 10 AND x <= 5 |
| `test_impossible_sum` | SUM(x) >= 1000, but only 6 boolean vars |
| `test_conflicting_aggregate` | SUM(x) >= 100 AND SUM(x) <= 1 |

All expect `packdb.InvalidInputException` matching "infeasible".

---

## 8. Adding a New Test

### Step 1: Choose the test file

Pick the file that matches the primary feature being tested (see section 5).
Create a new file if none fits.

### Step 2: Write the test function

```python
@pytest.mark.var_boolean          # variable type
@pytest.mark.cons_aggregate       # constraint category
@pytest.mark.obj_maximize         # objective type
@pytest.mark.correctness          # always include for oracle tests
def test_my_new_query(packdb_conn, duckdb_conn, oracle_solver, perf_tracker):
    # 1. Run DECIDE query
    sql = """
        SELECT col1, col2, x
        FROM my_table WHERE some_filter
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * col1) <= 100
        MAXIMIZE SUM(x * col2)
    """
    t0 = time.perf_counter()
    packdb_result = packdb_conn.execute(sql).fetchall()
    packdb_cols = [d[0] for d in packdb_conn.description]
    packdb_time = time.perf_counter() - t0

    # 2. Fetch same data via plain SQL
    data = duckdb_conn.execute("""
        SELECT CAST(col1 AS DOUBLE), CAST(col2 AS DOUBLE)
        FROM my_table WHERE some_filter
    """).fetchall()

    # 3. Build oracle model
    t_build = time.perf_counter()
    oracle_solver.create_model("my_test")
    vnames = [f"x_{i}" for i in range(len(data))]
    for vn in vnames:
        oracle_solver.add_variable(vn, VarType.BINARY)

    oracle_solver.add_constraint(
        {vnames[i]: data[i][0] for i in range(len(data))},
        "<=", 100.0, name="capacity",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][1] for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    build_time = time.perf_counter() - t_build
    result = oracle_solver.solve()

    # 4. Compare objectives
    assert_optimal_match(
        packdb_result, packdb_cols, result, ["x"],
        coeff_fn=lambda row: {"x": float(row[packdb_cols.index("col2")])},
    )

    # 5. Record performance
    perf_tracker.record(
        "my_test", packdb_time, build_time,
        result.solve_time_seconds, len(data), len(vnames), 1,
        result.objective_value, oracle_solver.solver_name(),
    )
```

### Step 3: Run

```bash
bash run_tests.sh -k "test_my_new_query" -v
```

---

## 9. Known Caveats

1. **Absolute tolerance on large objectives.** Comparison uses `|packdb - oracle| <= 1e-4`.
   For objectives in the millions, a relative tolerance might be more appropriate.
   No failures observed in practice.

2. **Session-scoped stateful solver.** The `oracle_solver` fixture is session-scoped.
   Each test calls `create_model()` which resets state, but if a test crashes mid-model
   the next test could inherit stale state.

3. **No feasibility verification.** `assert_optimal_match` checks objective equality
   but does not verify that PackDB's variable assignments satisfy the constraints.
   A future `constraint_checker` callback could be added.

4. **Fatal error cascading.** `test_nonlinear_decide_variables` triggers an
   `InternalException`. If PackDB's error handling leaves the connection in a bad
   state, subsequent tests sharing that connection may also fail. In practice this
   hasn't been an issue because connections are function-scoped.

5. **DECIDE subquery search path.** Subqueries inside SUCH THAT don't inherit the
   connection's `search_path`. Tables must be fully qualified (e.g. `tpch.customer`).

6. **No independent duckdb oracle.** Both `packdb_conn` and `duckdb_conn` use the
   `packdb` Python package because vanilla `duckdb` and `packdb` share pybind11
   type registrations and cannot coexist in one process. The `duckdb_conn` fixture
   is functionally equivalent for plain SQL, but it means both connections use the
   same engine.

7. **Wall-clock performance numbers.** Timings reflect wall-clock time including
   Python overhead, not CPU time. They are useful for relative comparisons across
   runs but not for absolute benchmarking.

8. **Untested Gurobi backend.** `gurobi_backend.py` is implemented but has not been
   validated (no Gurobi license available during development). All tests pass with
   HiGHS.

9. **TPC-H scale factor sensitivity.** Tests are tuned for SF-0.01 (`packdb.db`).
   Changing the scale factor may make some queries infeasible or shift constraint
   boundaries. Example: q09 uses `s_nationkey <= 5` to ensure 20+ suppliers.

10. **sys.path manipulation in conftest.** `conftest.py` inserts the test/decide
    directory onto `sys.path` so that `solver/`, `comparison/`, and `performance/`
    are importable. This is a convenience hack; a proper package install would be
    cleaner.
