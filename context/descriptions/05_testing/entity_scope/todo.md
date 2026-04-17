# Entity-Scope Test Coverage — Todo

## Missing coverage

### MEDIUM: NULL-semantics divergence between entity_scope and PER

NULL-keyed rows are handled *differently* by the two features:
- **Entity scope**: NULL entity-key columns collapse into a single shared "NULL entity" variable (covered by `test_entity_scoped_null_key`).
- **PER**: NULL-keyed rows are *excluded* from every group (see `_oracle_helpers.group_indices` and the PER grouping path in `physical_decide.cpp`).

Both behaviours are individually tested and internally consistent, but they diverge from each other. Decide whether the two should be aligned; if so, a test comparing them side-by-side on the same NULL-keyed dataset would catch regressions in whichever behaviour is chosen. See also [per/todo.md](../per/todo.md).
