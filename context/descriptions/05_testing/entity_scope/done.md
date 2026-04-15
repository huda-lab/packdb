# Entity-Scope Test Coverage — Done

Tests live in `test/decide/tests/test_entity_scope.py`.

## Oracle verification

Tests marked ✓ compare PackDB's objective value against an independent reference
solver. Tests marked (constraint only) verify feasibility and constraint
satisfaction but not optimality.

| Test | What it covers | Oracle |
|------|---------------|--------|
| `test_entity_scoped_nation_selection` | Basic BOOLEAN, MAXIMIZE SUM(keepN * acctbal), SUM constraint | ✓ |
| `test_entity_scoped_consistency` | Tight SUM <= 5, entity consistency under pressure | ✓ |
| `test_entity_scoped_integer` | IS INTEGER variable, per-row bound + aggregate bound | ✓ |
| `test_entity_scoped_mixed_with_row_scoped` | VarIndexer three-block layout: entity + row-scoped coexist | ✓ (per-customer vars) |
| `test_entity_scoped_with_when` | WHEN condition on constraint, entity-scoped | ✓ |
| `test_entity_scoped_nonexistent_table` | Error: scoping to nonexistent table | — (error test) |
| `test_entity_scoped_with_per` | PER grouping + entity-scoped, per-region oracle | ✓ |
| `test_entity_scoped_with_count` | COUNT(BOOLEAN) → SUM rewrite with entity-scoped | ✓ |
| `test_entity_scoped_with_max` | MAX(expr) <= K easy case, MIN/MAX linearization | ✓ |
| `test_entity_scoped_with_avg` | AVG → SUM scaling with entity-scoped | ✓ |
| `test_entity_scoped_when_per_triple` | WHEN + PER + entity-scoped triple interaction | ✓ |
| `test_entity_scoped_integer_count` | COUNT(INTEGER) → Big-M + entity-scoped indexing | constraint only |
| `test_entity_scoped_ne_constraint` | NE (`<>`) Big-M rewrite + entity-scoped | constraint only |
| `test_entity_scoped_max_hard_case` | MAX >= K hard case (Big-M) + entity-scoped | ✓ |
| `test_entity_scoped_mixed_when_per` | All-four: entity + row-scoped + WHEN + PER | constraint only |
| `test_entity_scoped_when_on_objective` | WHEN on objective + entity-scoped | ✓ |
| `test_entity_scoped_multi_column_per` | Multi-column PER (region × segment) + entity-scoped | ✓ |
| `test_entity_scoped_per_strict` | PER STRICT + entity-scoped | xfail (parser broken) |
| `test_entity_scoped_min_easy_case` | MIN >= K easy case + entity-scoped | ✓ |
| `test_entity_scoped_avg_per` | AVG + PER + entity-scoped | ✓ |
| `test_entity_scoped_ne_per` | NE + PER + entity-scoped | constraint only |
| `test_entity_scoped_between_constraint` | BETWEEN constraint + entity-scoped | constraint only |
| `test_entity_scoped_two_tables` | Two entity-scoped vars from different tables (n + r) | ✓ |
| `test_entity_scoped_var_in_when_condition_error` | Error: DECIDE var in WHEN condition | — (error test) |
| `test_entity_scoped_when_entity_invisible` | WHEN filters all rows for some entities | ✓ |
| `test_entity_scoped_equality_constraint` | Equality (=) constraint + entity-scoped | constraint only |
| `test_entity_scoped_is_real` | IS REAL entity-scoped, single-table, DOUBLE readback | ✓ |
| `test_entity_scoped_hard_min_max` | MIN <= K hard case + entity-scoped INTEGER | ✓ |
| `test_entity_scoped_abs` | ABS linearization + entity-scoped (per-row aux → entity) | ✓ |
| `test_entity_scoped_when_min_max_triple` | Entity-scoped + WHEN + hard MAX (triple interaction) | ✓ |
| `test_entity_scoped_ne_oracle` | Entity-scoped + `<>` (NE) Big-M with objective verification | ✓ |

## Silent-correctness bug fixed (2026-04-15)

While writing `test_entity_scoped_is_real` we found that entity-key columns
not otherwise referenced (SELECT/WHERE/constraints/objective) were pruned
from the table scan by the binder. A partial key survived and silently
collapsed distinct entities into region groups. Fix:
`TableBinding::GetColumnBinding` is now used at bind time to register every
entity-key column in the scan's column_ids; `LogicalDecide` stores a
`entity_key_expressions` vector of `BoundColumnRefExpression`s so the
column pruner's `VisitExpression` path rebinds them alongside other columns.
`plan_decide.cpp` refreshes `entity_scopes[...].entity_key_bindings` from
these expressions before resolving physical indices.

Files: `src/planner/binder/query_node/bind_select_node.cpp`,
`src/include/duckdb/planner/operator/logical_decide.hpp`,
`src/include/duckdb/planner/query_node/bound_select_node.hpp`,
`src/planner/binder/query_node/plan_select_node.cpp`,
`src/optimizer/remove_unused_columns.cpp`,
`src/execution/physical_plan/plan_decide.cpp`,
`src/include/duckdb/planner/table_binding.hpp`.

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| entity_scope | BOOLEAN | ✓ |
| entity_scope | INTEGER | ✓ |
| entity_scope | row-scoped (mixed) | ✓ |
| entity_scope | WHEN (constraint) | ✓ |
| entity_scope | WHEN (objective) | ✓ |
| entity_scope | PER (single-column) | ✓ |
| entity_scope | PER (multi-column) | ✓ |
| entity_scope | WHEN + PER | ✓ |
| entity_scope | COUNT (BOOLEAN → SUM) | ✓ |
| entity_scope | COUNT (INTEGER → Big-M) | ✓ |
| entity_scope | MAX easy (≤ K) | ✓ |
| entity_scope | MAX hard (≥ K) | ✓ |
| entity_scope | MIN easy (≥ K) | ✓ |
| entity_scope | AVG (standalone) | ✓ |
| entity_scope | AVG + PER | ✓ |
| entity_scope | NE (`<>`) | ✓ |
| entity_scope | NE + PER | ✓ |
| entity_scope | BETWEEN | ✓ |
| entity_scope | two entity-scoped tables | ✓ (oracle) |
| entity_scope | WHEN filters entity to zero | ✓ |
| entity_scope | PER STRICT | xfail |
| entity_scope + row-scoped | WHEN + PER | ✓ |
