# PackDB

PackDB extends DuckDB with a DECIDE clause for in-database Integer Linear Programming.
See `context/descriptions/` for full documentation (start with `README.md` there).

## Build

```bash
make release                         # Linux release build
```

Build output: `build/release/packdb` (CLI), `build/release/src/libduckdb.so`

## Test

```bash
make decide-test                       # Run DECIDE differential tests
make decide-setup                      # Setup test environment only
```
Tests are in `test/decide/`.

## Benchmark

```bash
make decide-bench-setup                # Generate TPC-H databases (small/medium/large)
make decide-bench                      # Run all queries × all sizes (with stage timers)
make decide-bench-manual               # Run custom query from queries/manual.sql
make decide-view                       # View latest results (colored stage bars)
```

Selective: `python3 benchmark/decide/run_benchmarks.py --sizes small --queries Q1,Q3 --compare`

**Optimization loop**: Use `/bench` to automate build → benchmark (medium) → analyze stages → suggest next optimization. Full docs: `context/descriptions/02_operations/benchmarking.md`.

## Key PackDB source paths

- Parser/Symbolic: `src/packdb/symbolic/decide_symbolic.cpp`
- Binder: `src/planner/expression_binder/decide_binder.cpp`, `decide_constraints_binder.cpp`, `decide_objective_binder.cpp`
- Logical operator: `src/planner/operator/logical_decide.cpp`
- Optimizer: `src/optimizer/decide/decide_optimizer.cpp` (algebraic rewrites: COUNT→SUM, AVG→SUM, ABS linearization, MIN/MAX classification, `<>` indicators)
- Physical execution + solver integration (Gurobi/HiGHS): `src/execution/operator/decide/physical_decide.cpp`
- Headers: `src/include/duckdb/` (see `common/enums/decide.hpp`, `planner/operator/logical_decide.hpp`, `optimizer/decide_optimizer.hpp`, etc.)

## DECIDE Syntax (Quick Reference)

```sql
SELECT select_list
FROM table_expression
[WHERE ...]
DECIDE variable_name [IS type] [, ...]
SUCH THAT constraint [AND constraint ...]
[MAXIMIZE | MINIMIZE] SUM|MIN|MAX(linear_expression)
```

- Variable types: `IS BOOLEAN` (0/1), `IS INTEGER` (default, non-negative), `IS REAL` (continuous, non-negative)
- Constraints: linear expressions with `=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`, `IN` (all operators supported on both per-row and aggregate constraints; `IN` on aggregates not supported)
- Conditional: `expression WHEN condition` (postfix, on constraints and objectives)
- Grouping: `SUM(expr) op rhs PER column` or `PER (col1, col2, ...)` (one constraint per distinct value/combination)
- `SUM()` aggregate supported over decision variables; `COUNT(x)` supported for BOOLEAN (rewritten to SUM) and INTEGER (Big-M indicator variable rewrite); `AVG(expr)` supported (rewritten to SUM with RHS scaling by row count at execution time)
- `MIN(expr)` / `MAX(expr)` supported in constraints and objectives via linearization:
  - **Easy cases** (no Big-M): `MAX(expr) <= K` → per-row `expr <= K`; `MIN(expr) >= K` → per-row `expr >= K`. The aggregate is simply stripped because bounding every row individually is equivalent.
  - **Hard cases** (Big-M indicators): `MAX(expr) >= K`, `MIN(expr) <= K`, equality. These require a binary indicator variable per row because "at least one row satisfies" is a disjunctive constraint that can't be expressed linearly without Big-M.
  - **Easy objectives**: `MINIMIZE MAX(expr)`, `MAXIMIZE MIN(expr)` → global auxiliary variable `z` with per-row linking constraints (`z >= expr_i` or `z <= expr_i`).
  - **Hard objectives**: `MAXIMIZE MAX(expr)`, `MINIMIZE MIN(expr)` → global `z` + per-row binary indicators + `SUM(y) >= 1`, because finding the row that achieves the optimum requires indicator selection.
- **PER on objectives**: Nested aggregate syntax `OUTER(INNER(expr)) PER col` where OUTER/INNER ∈ {SUM, MIN, MAX, AVG}. AVG as outer maps to SUM (constant divisor). AVG as inner scales coefficients by `1/n_g` (group size) — NOT equivalent to SUM when groups have different sizes. Flat `SUM/AVG + PER` is a no-op; flat `MIN/MAX + PER` is an error (ambiguous). Two-level ILP formulation: inner creates per-group auxiliaries, outer creates global auxiliary. Easy/hard classification applies at each level independently.
- All expressions must be linear (no `x * y` between decision variables)

For full syntax details: `context/descriptions/00_project_overview/syntax_reference.md`
For keyword-by-keyword reference: `context/descriptions/03_expressivity/`

## Core Principles

- **Follow DuckDB patterns first**: When adding a feature, find how DuckDB handles the analogous SQL case and mirror that approach. Don't invent new patterns when DuckDB already has one.
- **Solver-agnostic**: Features must work with both Gurobi and HiGHS. Don't rely on solver-specific capabilities without a fallback path.
- **Minimal DuckDB core modifications**: PackDB extends DuckDB; prefer adding new code over modifying core DuckDB files. The less we touch upstream, the easier version upgrades are.

## Demand Elegance (Balanced)

- For non-trivial changes: pause and ask "is there a more elegant way?" before presenting
- If a fix feels hacky: step back and implement the elegant solution
- Skip this for simple, obvious fixes — don't over-engineer

## Conventions

- PackDB code follows DuckDB coding conventions (CamelCase classes, snake_case methods)
- Libraries are named `libduckdb.*` internally for DuckDB API compatibility
- The executable is named `packdb`
- DECIDE clause keywords: DECIDE, SUCH THAT, MAXIMIZE, MINIMIZE, WHEN
- WHEN is postfix on constraints and objectives: `expression WHEN condition` (not `WHEN condition THEN expression`)
- Only linear constraints/objectives supported (no quadratic)
- Solver strategy: Gurobi (primary, commercial) — empirically much faster in practice. HiGHS (bundled, open-source) is retained as a fallback only; it is significantly slower and not recommended for production use.
- Always use `python3` (not `python`) — `python` is not available on this system

## Grammar Changes

Editing `third_party/libpg_query/grammar/` `.y`/`.yh` files requires regeneration before building:
```bash
make grammar-build                     # regenerate grammar + rebuild (one step)
make grammar                           # regenerate grammar only (requires bison 2.3)
```
The `.y` files are templates; the actual compiled parser is `third_party/libpg_query/src_backend_parser_gram.cpp` (generated).

## Lessons

See `.claude/lessons.md` for corrections and gotchas discovered during development. Update it after any mistake to prevent recurrence.

## Development Priorities

1. **Performance**: Use `/bench` for the optimize-measure loop. Solver dominates (~95% at scale). Focus on reducing solver input size and improving formulations.
   - See `context/descriptions/02_operations/benchmarking.md`
2. **Expressivity**: See `context/descriptions/03_expressivity/` (each keyword has `done.md`/`todo.md`)
3. **Optimizer**: Big-M reformulation, push-down / pull-out rewrites
   - See `context/descriptions/04_optimizer/` (each strategy area has `done.md`/`todo.md`)

## Documentation

Full docs in `context/descriptions/` — start with `README.md` there for navigation and reading order.
Key areas: `00_project_overview/` (syntax spec), `01_pipeline/` (architecture), `03_expressivity/` (feature status), `04_optimizer/` (rewrite strategies).

**MANDATORY: Keep docs in sync with code changes.** Whenever a code change affects the behavior, semantics, or implementation of a feature documented in `context/descriptions/`, you MUST update the relevant `done.md` (and `todo.md` if applicable) in the same work session. This includes:
- Semantic changes (how a feature works)
- Implementation changes (data structures, code paths, function signatures)
- Code Pointers sections (line numbers, file references, tag constants)
- New feature interactions (e.g., WHEN+PER composition)

If unsure which doc to update, check `context/descriptions/README.md` for the directory layout. Ask the user for confirmation if which doc to update is not clear or if a new doc may be needed.
