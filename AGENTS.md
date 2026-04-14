# PackDB Codex Instructions

PackDB extends DuckDB with a DECIDE clause for in-database Integer Linear Programming.
Use `context/descriptions/` as the source of truth for project documentation; start with `context/descriptions/README.md`.

This file is the Codex entrypoint for this repository. Leave `.claude/` untouched; it supports Claude Code separately.

## Build

```bash
make release
```

Build output: `build/release/packdb` and `build/release/src/libduckdb.so`.

## Test

```bash
make decide-test
make decide-setup
```

Tests are in `test/decide/`.

## Benchmark

```bash
make decide-bench-setup
make decide-bench
make decide-bench-manual
make decide-view
```

Selective benchmark:

```bash
python3 benchmark/decide/run_benchmarks.py --sizes small --queries Q1,Q3 --compare
```

Use `$packdb-bench` for the automated build, benchmark, analyze, and suggest loop.

## Repo-Local Codex Workflows

These workflows are repo-local files, not globally installed Codex skills. When the latest user request invokes one of these names, read the matching file before acting and parse the remaining text as that workflow's arguments:

- `$packdb-bench`: `.codex/skills/packdb-bench/SKILL.md`
- `$packdb-review`: `.codex/skills/packdb-review/SKILL.md`
- `$packdb-recap`: `.codex/skills/packdb-recap/SKILL.md`
- `$packdb-test-review`: `.codex/skills/packdb-test-review/SKILL.md`
- `$packdb-learn`: `.codex/skills/packdb-learn/SKILL.md`
- `$packdb-docs-review`: `.codex/skills/packdb-docs-review/SKILL.md`

Do not search `~/.codex/skills` for these PackDB workflows unless the user explicitly asks to install or globalize them.

## Key PackDB Source Paths

- Parser/Symbolic: `src/packdb/symbolic/decide_symbolic.cpp`
- Binder: `src/planner/expression_binder/decide_binder.cpp`, `src/planner/expression_binder/decide_constraints_binder.cpp`, `src/planner/expression_binder/decide_objective_binder.cpp`
- Logical operator: `src/planner/operator/logical_decide.cpp`
- Optimizer: `src/optimizer/decide/decide_optimizer.cpp`
- Physical execution and solver integration: `src/execution/operator/decide/physical_decide.cpp`
- Headers: `src/include/duckdb/`, especially `common/enums/decide.hpp`, `planner/operator/logical_decide.hpp`, and `optimizer/decide_optimizer.hpp`

## DECIDE Syntax Quick Reference

```sql
SELECT select_list
FROM table_expression
[WHERE ...]
DECIDE [Table.]variable_name [IS type] [, ...]
SUCH THAT constraint [AND constraint ...]
[MAXIMIZE | MINIMIZE] SUM|MIN|MAX(linear_expression)
[MINIMIZE] SUM(POWER(linear_expression, 2))
[MAXIMIZE] SUM(-POWER(linear_expression, 2))
[MAXIMIZE] SUM(POWER(linear_expression, 2))
[MAXIMIZE|MINIMIZE] SUM(bool_var * other_var)
[MAXIMIZE|MINIMIZE] SUM(var_a * var_b)
```

- Variable types: `IS BOOLEAN` (0/1), `IS INTEGER` (default, non-negative), `IS REAL` (continuous, non-negative).
- Table-scoped variables: `DECIDE Table.var IS TYPE`; one variable per unique entity in the source table. The qualifier must match an alias or table name in the `FROM` clause.
- Constraints: linear expressions with `=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`, and `IN`. `IN` on aggregates is not supported.
- Conditional: postfix `expression WHEN condition` on constraints and objectives.
- Grouping: `SUM(expr) op rhs PER column` or `PER (col1, col2, ...)`.
- Aggregates: `SUM`, `COUNT`, `AVG`, `MIN`, and `MAX` are supported over decision expressions according to the docs under `context/descriptions/03_expressivity/`.
- Quadratic objectives: `POWER(expr, 2)`, `expr ** 2`, and `(expr) * (expr)` are supported with the solver restrictions documented in `context/descriptions/03_expressivity/problem_types/done.md`.
- Bilinear terms: Boolean times anything is linearized with McCormick/AND constraints; general non-convex products require Gurobi where supported.

For the full syntax spec, read `context/descriptions/00_project_overview/syntax_reference.md`.

## Core Principles

- Follow DuckDB patterns first. Find the analogous DuckDB SQL path before adding a new pattern.
- Keep features solver-agnostic across Gurobi and HiGHS unless the docs explicitly define a solver-specific restriction.
- Keep DuckDB core modifications minimal. Prefer PackDB extension code over upstream-style core churn.
- For non-trivial changes, pause and ask whether there is a cleaner formulation before presenting or finalizing.

## Conventions

- PackDB follows DuckDB C++ conventions: CamelCase classes and snake_case methods.
- Libraries are named `libduckdb.*` internally for DuckDB API compatibility.
- The executable is named `packdb`.
- DECIDE keywords are `DECIDE`, `SUCH THAT`, `MAXIMIZE`, `MINIMIZE`, and `WHEN`.
- `WHEN` is postfix: `expression WHEN condition`, not `WHEN condition THEN expression`.
- Always use `python3`, not `python`.

## Grammar Changes

Editing `third_party/libpg_query/grammar/` `.y` or `.yh` files requires regeneration before building:

```bash
make grammar-build
make grammar
```

The `.y` files are templates. The compiled parser is `third_party/libpg_query/src_backend_parser_gram.cpp`.

## Lessons

Read `.codex/lessons.md` for PackDB gotchas discovered during development. Update it after a mistake or repeated gotcha to prevent recurrence.

## Development Priorities

1. Performance: use `$packdb-bench` for the optimize-measure loop. Solver time dominates at scale; focus on reducing solver input size and improving formulations.
2. Expressivity: read `context/descriptions/03_expressivity/`.
3. Optimizer: read `context/descriptions/04_optimizer/`.

## Documentation Sync

Keep docs in sync with code changes. Whenever a code change affects behavior, semantics, or implementation documented in `context/descriptions/`, update the relevant `done.md` and `todo.md` in the same work session.

This includes semantic changes, implementation changes, code pointer drift, new feature interactions, and changed restrictions. If the right doc is unclear, use `context/descriptions/README.md` to find the matching area and ask the user only if the choice remains ambiguous.
