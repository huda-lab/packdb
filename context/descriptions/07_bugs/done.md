# Bugs — Fixed

Log of bugs that were discovered and resolved. Kept for history; active bugs live in `todo.md`.

---

## Table-Scoped DECIDE Variables Cannot Be Projected as `Table.var` In The SELECT List (and per-row LHS)

### Symptom

Queries declaring a table-scoped DECIDE variable (e.g. `DECIDE supplier.pick IS BOOLEAN`) failed at bind time when the variable was referenced through its table qualifier outside the DECIDE-internal binders:

```
Binder Error: Table "supplier" does not have a column named "pick"
```

Two distinct call sites hit this:

1. `Table.var` in the SELECT list — `SELECT supplier.pick FROM supplier DECIDE supplier.pick IS BOOLEAN ...`
2. `Table.var` as the LHS of a *per-row* SUCH THAT constraint — `SUCH THAT supplier.pick <= 1 ...`

The DECIDE / SUCH THAT (aggregate) / MAXIMIZE / MINIMIZE clauses themselves accepted the qualified form fine, so the failure shape was confusing to users who had qualified the variable consistently.

### Root cause

Two binders were seeing two different name spaces.

`bind_select_node.cpp` registers each table-scoped variable under both its unqualified name (`pick`) and its qualified name (`supplier.pick`) in a `decide_variable_names` map. That map is consulted by `DecideConstraintsBinder` and `DecideObjectiveBinder`, so SUCH THAT / objective references resolve correctly. But two other paths bypassed the map:

- The SELECT list is bound by the *regular* DuckDB binder. It only sees decision variables through the generic bind-context binding `decide_variables` (added at `bind_select_node.cpp:814`), which exposes only unqualified `var_names`. When given `supplier.pick`, it routed `supplier` to the real `TableBinding` for the supplier table, which has no `pick` column.
- The per-row branch of `DecideConstraintsBinder` (`decide_constraints_binder.cpp:182`) falls through to `ExpressionBinder::BindExpression` for column refs — same regular DuckDB binder, same failure mode. The aggregate-SUM branch only worked because `NormalizeDecideConstraints` round-trips the LHS through SymEngine (`decide_symbolic.cpp:257` / `:778`), which silently drops table qualifiers as a side effect.

### Fix

Added a single parsed-AST pre-pass `RewriteScopedVarRefs` in `bind_select_node.cpp` that walks an expression tree and rewrites any qualified `ColumnRefExpression` whose `Table.col` form matches a registered scoped DECIDE variable into a bare `ColumnRefExpression(col)`. The rewrite is applied to:

- `statement.decide_constraints`
- `statement.decide_objective`
- each element of `statement.select_list`

It runs immediately after `decide_variable_names` is fully built, before `RewriteInDomain`, normalization, and binding. After this pass, every reference to a scoped decision variable is unqualified, so the regular DuckDB binder (SELECT) and the per-row branch of `DecideConstraintsBinder` both resolve them through the existing generic `decide_variables` binding. The aggregate-SUM path is unaffected — it already arrived at the bare form via SymEngine; the rewrite just gets there earlier and uniformly.

The pass is a no-op when no scoped variables are declared, and bare `ColumnRefExpression`s are skipped, so unrelated qualified refs (e.g. `supplier.s_acctbal` for a real column) are untouched.

### Verification

- `make decide-test` — 547 passed, 0 failed.
- Stress-query repros restored to working: P13, P14 (`stress_queries/04_problem_classes.sql`); V8–V13 (`stress_queries/06_variables.sql`); per-row qualified LHS (V12), alias-qualified scope (V9), bilinear over two scoped tables (V13).

### Code pointers

- Helper: `src/planner/binder/query_node/bind_select_node.cpp` (`RewriteScopedVarRefs`)
- Wire-in site: same file, immediately after `decide_variable_names` is built and before `RewriteInDomain`
- Background — qualified-name registration: same file (`decide_variable_names.emplace(qualified_name, var_idx)`)
- Background — generic SELECT binding alias: same file (`bind_context.AddGenericBinding(result->decide_index, "decide_variables", var_names, var_types)`)
- Background — per-row fall-through: `src/planner/expression_binder/decide_constraints_binder.cpp` (`return ExpressionBinder::BindExpression(expr_ptr, depth)`)
- Background — accidental qualifier strip on aggregate path: `src/packdb/symbolic/decide_symbolic.cpp` (`ToSymbolicRecursive` reads only `colref.GetColumnName()`; `FromSymbolic` rebuilds unqualified)
