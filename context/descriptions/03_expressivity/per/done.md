# PER Keyword — Implemented Features

## Motivation

Many data manipulation tasks require **one constraint per data-driven group** — but the groups are not known when the query is written. Without `PER`, users must write one explicit constraint per group, which is impractical when groups are determined by the data.

**Example — per-employee workload cap**: Each employee must work at most 40 hours/week. Without `PER`:

```sql
-- Impractical without PER:
SUCH THAT
    SUM(new_hours) <= 40 WHEN empID = 'E001' AND
    SUM(new_hours) <= 40 WHEN empID = 'E002' AND
    ...   -- one per employee (unknown count)
```

With `PER`:

```sql
SUCH THAT
    SUM(new_hours) <= 40 PER empID
```

---

## Core PER on Constraints

PER groups aggregate constraints by distinct column values. One ILP constraint is generated per distinct value of the PER column.

### Syntax

```sql
-- Constraint (single-column PER only, multi-column deferred)
constraint_expression [WHEN condition] PER column

-- Objective (accepted, treated as global SUM — see Deferred Features in todo.md)
MAXIMIZE SUM(...) PER column
```

**Example**:

```sql
-- One constraint per distinct empID
SUCH THAT SUM(x * hours) <= 40 PER empID
```

### Semantics

`PER column` causes the system to:
1. Find all distinct values of `column` in the input relation (after any `WHEN` filter).
2. Generate one copy of the constraint for each distinct value, applying that value as an implicit filter.

The constraint `SUM(new_hours) <= 40 PER empID` is semantically equivalent to:

```sql
SUM(new_hours) <= 40 WHEN empID = 'E001' AND
SUM(new_hours) <= 40 WHEN empID = 'E002' AND
SUM(new_hours) <= 40 WHEN empID = 'E003' AND
...
```

### WHEN + PER Composition

WHEN filters rows first, then PER groups the remaining rows:

```sql
-- Per-employee constraint, but only for Directors
SUCH THAT SUM(x * hours) <= 30 WHEN title = 'Director' PER empID
```

**Execution order**: WHEN (filter) → PER (partition → generate one constraint per group).

### PER on Objective (No-Op)

PER is accepted on objectives by the grammar and binder, but currently treated as equivalent to the global SUM (no-op). This will become meaningful when partition-solve is implemented.

```sql
-- Currently equivalent to: MINIMIZE SUM(x * cost)
MINIMIZE SUM(x * cost) PER department
```

---

## Restrictions

- **Aggregate-only**: PER requires a SUM constraint. Per-row constraints produce a binder error.
- **Single-column**: Only `PER column` (not `PER (col1, col2)`).
- **Constant RHS**: No row-varying RHS with PER.
- **Table columns only**: PER column must be a table column, not a DECIDE variable or expression.
- **NULL handling**: NULL PER values exclude the row (`INVALID_INDEX`), matching SQL GROUP BY behavior.

---

## Architecture: Unified WHEN + PER via `row_group_ids`

PER and WHEN are unified under a single abstraction. Instead of separate `row_mask` (WHEN) and group information (PER), the system uses one field:

```cpp
// In EvaluatedConstraint (solver_input.hpp):
vector<idx_t> row_group_ids;   // Unified WHEN+PER: row→group mapping
idx_t num_groups = 0;           // 0 = ungrouped, >0 = number of groups
```

| Case | `row_group_ids` | `num_groups` | ILP constraints |
|------|-----------------|-------------|-----------------|
| No WHEN, no PER | empty | 0 | 1 (all rows) |
| WHEN only | 0 or INVALID_INDEX | 1 | 1 (matching rows) |
| PER only | 0..K-1 | K | K (one per group) |
| WHEN + PER | 0..K-1 or INVALID_INDEX | K | K (filtered, grouped) |

`ILPModel::Build` uses a group→rows index for O(N)-total constraint generation across all groups.

---

## Use Cases

### Per-Employee / Per-Group Resource Limits (Repair)

```sql
SELECT * DECIDE new_hours IS INTEGER
FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
SUCH THAT
    SUM(new_hours) <= 40 PER P.empID AND
    SUM(new_hours) <= 30 WHEN E.title = 'Director' PER P.empID
MINIMIZE SUM(ABS(new_hours - hours)) PER projectID
```

### Per-Label Coverage (Active Learning / Selection)

```sql
SELECT * DECIDE keep IS BOOLEAN FROM Reviews
SUCH THAT
    SUM(keep * weak_label) >= 50 PER weak_label AND
    SUM(keep) BETWEEN 200 AND 400
MINIMIZE SUM(keep * confidence)
```

Here `PER weak_label` generates one coverage constraint per distinct label class, ensuring adequate representation.

---

## Scaling Considerations

The number of generated constraints equals `|distinct_values| x |PER_constraints|`. For large relations this can produce O(|D|) constraints (one per tuple in the worst case). This is a key motivation for the optimizer's constraint reduction strategies (see [../../04_optimizer/problem_reduction/todo.md](../../04_optimizer/problem_reduction/todo.md)).

**Mitigation strategies** (future optimizer work):
- **Partition-solve**: When all constraints and objective share the same PER column and there are no global constraints, decompose into K independent ILPs — see [../../04_optimizer/problem_reduction/todo.md](../../04_optimizer/problem_reduction/todo.md)
- **Constraint-to-bound conversion**: Detect PER constraints that are equivalent to simple variable bounds
- **Skyband pruning**: Eliminate dominated tuples before generating constraints
- **Drop-solve-validate-refine loop**: Generate a subset of constraints, solve, check if dropped constraints are violated, and iteratively refine

---

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
