# PER Keyword — Implemented Features

## Core PER on Constraints

PER groups aggregate constraints by distinct column values. One ILP constraint is generated per distinct value of the PER column.

**Syntax**: `SUM(expr) comparison rhs PER column`

```sql
-- One constraint per distinct empID
SUCH THAT SUM(x * hours) <= 40 PER empID
```

### WHEN + PER Composition

WHEN filters rows first, then PER groups the remaining rows:

```sql
-- Per-employee constraint, but only for Directors
SUCH THAT SUM(x * hours) <= 30 WHEN title = 'Director' PER empID
```

### PER on Objective (No-Op)

PER is accepted on objectives by the grammar and binder, but currently treated as equivalent to the global SUM (no-op). This will become meaningful when partition-solve is implemented.

```sql
-- Currently equivalent to: MINIMIZE SUM(x * cost)
MINIMIZE SUM(x * cost) PER department
```

## Architecture

PER and WHEN are unified under `row_group_ids` in `EvaluatedConstraint` (replaces the old `row_mask`). See [todo.md](todo.md) for the full architecture description.

## Restrictions

- **Aggregate-only**: PER requires a SUM constraint. Per-row constraints produce a binder error.
- **Single-column**: Only `PER column` (not `PER (col1, col2)`).
- **Constant RHS**: No row-varying RHS with PER.
- **Table columns only**: PER column must be a table column, not a DECIDE variable or expression.
- **NULL handling**: NULL PER values exclude the row (`INVALID_INDEX`).

## Files Modified

- `src/include/duckdb/common/enums/decide.hpp` — `PER_CONSTRAINT_TAG`
- `src/include/duckdb/execution/operator/decide/physical_decide.hpp` — `LinearConstraint::per_column`
- `src/include/duckdb/packdb/solver_input.hpp` — `row_group_ids` replaces `row_mask`
- `third_party/libpg_query/` — grammar rules, keyword, enum
- `src/parser/transform/expression/transform_operator.cpp` — transformer
- `src/packdb/symbolic/decide_symbolic.cpp` — normalizer passthrough
- `src/planner/expression_binder/decide_constraints_binder.cpp/.hpp` — `BindPerConstraint`
- `src/planner/expression_binder/decide_objective_binder.cpp` — PER strip (no-op)
- `src/execution/operator/decide/physical_decide.cpp` — unified WHEN+PER evaluation
- `src/packdb/utility/ilp_model_builder.cpp` — group-aware constraint builder
