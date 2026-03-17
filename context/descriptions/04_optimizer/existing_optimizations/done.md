# Existing Optimizations

Everything currently implemented that reduces ILP size, improves solve time, or transforms the problem before it reaches the solver.

---

## 1. WHERE-Clause Filtering (Inherited from DuckDB)

Standard DuckDB predicate pushdown. Rows eliminated by WHERE never enter the constraint/objective matrix. This is not a COP-specific optimization — it's inherited from DuckDB's query processing pipeline — but it is the single most impactful optimization for most queries, since it reduces the number of decision variables (one per row).

**Example**: `SELECT ... FROM items WHERE price < 100 DECIDE x IS BOOLEAN ...` — only rows with `price < 100` become decision variables.

**Code**: No DECIDE-specific code; DuckDB's standard filter pushdown handles this before rows reach `PhysicalDecide`.

---

## 2. WHEN-Condition Coefficient Zeroing

When a constraint or objective has a `WHEN` modifier, rows that fail the condition are effectively excluded without removing them from the variable set.

**Mechanism**:
- WHEN condition evaluated per-row to produce a boolean mask
- **Aggregate constraints**: coefficients multiplied by mask (0 for non-matching rows), so excluded rows contribute nothing to the aggregate
- **Per-row constraints**: constraint row omitted entirely for non-matching rows
- **Objectives**: objective coefficient zeroed for non-matching rows

This interacts with PER via the unified `row_group_ids` architecture: WHEN-excluded rows get `INVALID_INDEX` in their group assignment.

**Code pointers**:
- Objective coefficient zeroing: `src/execution/operator/decide/physical_decide.cpp:1007-1039`
- Constraint row filtering (WHEN mask → row_group_ids): `src/execution/operator/decide/physical_decide.cpp:820-852`

---

## 3. Solver Selection (Gurobi / HiGHS Fallback)

Static dispatch: try Gurobi first, fall back to HiGHS if unavailable.

- **Gurobi**: Commercial (free academic license), significantly faster on large ILPs, uses C API
- **HiGHS**: Open-source, bundled with PackDB, slower but always available

Selection is **not** cost-based — Gurobi is always preferred regardless of problem characteristics. See `future_work/todo.md` for cost-based selection plans.

**Code pointers**:
- Dispatch logic: `src/packdb/utility/ilp_solver.cpp:8-15`
- Gurobi backend: `src/packdb/gurobi/gurobi_solver.cpp` (~275 lines)
  - `IsAvailable()` checks for Gurobi library at runtime
  - `Solve()` builds Gurobi model, adds variables/constraints/objective, calls `GRBoptimize()`
- HiGHS backend: `src/packdb/naive/deterministic_naive.cpp` (~189 lines)
  - Uses `Highs` C++ API, builds `HighsLp` with constraint matrix in CSC format
  - `highs.setOptionValue("log_to_console", false)` disables console output
- Model builder: `src/packdb/utility/ilp_solver.cpp` — `ILPModel::Build()` converts `SolverInput` to solver-agnostic matrix

---

## 4. Binder-Level Algebraic Rewrites

These transforms run during binding (before the optimizer) and rewrite DECIDE expressions into equivalent linear forms. They are "proto-optimizations" — necessary for correctness (the solver requires linear input) but also reduce problem size when possible.

### COUNT to SUM Rewrite

**BOOLEAN variables**: `COUNT(x)` becomes `SUM(x)` directly, since a BOOLEAN variable is 0 or 1 and counting non-zero values is equivalent to summing.

**INTEGER variables**: `COUNT(x)` becomes `SUM(__count_ind_x__)` where `__count_ind_x__` is a new BOOLEAN indicator variable. Big-M constraints link the indicator to the original variable (indicator = 1 iff variable > 0). This is more expensive (adds one binary variable + Big-M constraints per original variable).

**Code**: `src/planner/binder/query_node/bind_select_node.cpp:414-467` (`RewriteCountToSum`)
**Optimizer linkage**: `src/optimizer/decide/decide_optimizer.cpp:84-103` (`DecideOptimizer::RewriteCount` builds `count_indicator_links`)

### AVG to SUM Rewrite

`AVG(expr) op K` is rewritten to `SUM(expr) op K` at the expression level, with a flag (`was_avg_rewrite`) marking the constraint. At execution time, the RHS is scaled by the row count (or group size for PER): `AVG(expr) <= K` becomes `SUM(expr) <= K * count`.

**Code**:
- Flag setting: `src/execution/operator/decide/physical_decide.cpp:391`
- RHS scaling (non-PER): `src/packdb/utility/ilp_model_builder.cpp:164-166`
- RHS scaling (PER): `src/packdb/utility/ilp_model_builder.cpp:211-213`

### ABS Linearization

`ABS(expr)` is replaced by an auxiliary REAL variable `__abs_N__`. Two linearization constraints are generated: `aux >= expr` and `aux >= -expr`. Together these force `aux = |expr|`.

**Code**:
- Binder (auxiliary variable creation): `src/planner/binder/query_node/bind_select_node.cpp:527-546` (`RewriteAbsLinearization`)
- Optimizer (constraint generation): `src/optimizer/decide/decide_optimizer.cpp:105-136` (`DecideOptimizer::RewriteAbs`)

### MIN/MAX Linearization

Aggregate `MIN(expr)` and `MAX(expr)` in constraints and objectives are classified as easy or hard:

- **Easy cases** (no Big-M needed): `MAX(expr) <= K` becomes per-row `expr <= K`; `MIN(expr) >= K` becomes per-row `expr >= K`. The aggregate is stripped because bounding every row individually is equivalent.
- **Hard cases** (Big-M indicators): `MAX(expr) >= K`, `MIN(expr) <= K`, equality. These require a binary indicator variable per row because "at least one row satisfies" is a disjunctive constraint.
- **Easy objectives**: `MINIMIZE MAX(expr)`, `MAXIMIZE MIN(expr)` use a global auxiliary variable with per-row linking constraints.
- **Hard objectives**: `MAXIMIZE MAX(expr)`, `MINIMIZE MIN(expr)` use a global auxiliary + per-row binary indicators + `SUM(y) >= 1`.

**Code**: `src/planner/binder/query_node/bind_select_node.cpp:559+` (`RewriteMinMaxInExpression`)

### Not-Equal (`<>`) Disjunction

`x <> K` is a disjunctive constraint (x < K OR x > K) which is not directly expressible in linear programming. Rewritten using a binary indicator variable `__ne_ind_N__` and Big-M constraints.

**Code**: `src/optimizer/decide/decide_optimizer.cpp:38-47` (`DecideOptimizer::RewriteNotEqual`)

### IN on Decision Variables

`x IN (v1, v2, ...)` is rewritten using auxiliary binary indicator variables, one per value in the set. Constraints enforce that exactly one indicator is active and the variable takes the corresponding value.

---

## 5. DecideOptimizer Pass

The optimizer runs three sub-passes on `LogicalDecide` nodes after binding:

1. `RewriteNotEqual` — creates `<>` indicator variables
2. `RewriteCount` — builds count_indicator_links (connecting binder-created indicators to original variables)
3. `RewriteAbs` — generates ABS linearization constraints from binder-stashed expressions

**Entry point**: `src/optimizer/decide/decide_optimizer.cpp:32-36` (`DecideOptimizer::OptimizeDecide`)
**Header**: `src/include/duckdb/optimizer/decide_optimizer.hpp`
**Registration**: `src/optimizer/optimizer.cpp` (integrated into DuckDB's optimizer pipeline)
