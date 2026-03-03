# PER Keyword — Design and Implementation Plan

**Priority: High** (enables the majority of real-world use cases beyond simple selection)

---

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

## Syntax

```sql
-- Constraint (single-column PER only, multi-column deferred)
constraint_expression [WHEN condition] PER column

-- Objective (accepted, treated as global SUM — see Deferred Features)
MAXIMIZE SUM(...) PER column
```

---

## Semantics

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

---

## Interaction with WHEN

`WHEN` and `PER` combine: `WHEN` filters rows first, then `PER` groups the filtered rows.

```sql
-- One constraint per empID, but only counting Director rows
SUM(new_hours) <= 30 WHEN title = 'Director' PER empID
```

**Execution order**: WHEN (filter) -> PER (partition -> generate one constraint per group).

---

## Restrictions (Current Implementation)

- **Aggregate-only**: PER is only valid on aggregate (SUM) constraints. Per-row constraints like `x <= 5 PER col` produce a binder error.
- **Single-column**: Only `PER column` is supported. Multi-column `PER (col1, col2)` is deferred.
- **Constant RHS**: The right-hand side of a PER constraint must be constant. Row-varying RHS (`SUM(x) <= budget PER dept`) is not supported.
- **Table columns only**: PER column must be a table column reference, not a DECIDE variable or computed expression.
- **NULL handling**: Rows where the PER column is NULL are excluded (receive `INVALID_INDEX` group assignment), matching SQL GROUP BY behavior.

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

## Deferred Features

### Multi-Column PER

```sql
-- NOT YET SUPPORTED
SUM(x) <= 40 PER (empID, department)
```

Requires grammar support for parenthesized column lists (comma conflicts with constraint separator), and multi-column group ID assignment. The `row_group_ids` architecture supports this natively — just compute group IDs from the combination of column values.

### PER on Objective (Partition-Solve Semantics)

PER on the objective is accepted by the grammar and binder but currently treated as equivalent to global SUM (no-op). This becomes meaningful when partition-solve is implemented:

```sql
-- Currently treated as global MINIMIZE SUM(...)
-- Future: decompose into independent per-group optimization
MINIMIZE SUM(ABS(new_hours - hours)) PER projectID
```

See [../../04_optimizer/problem_reduction/todo.md](../../04_optimizer/problem_reduction/todo.md) for the partition-solve design.

### Row-Varying RHS with PER

```sql
-- NOT YET SUPPORTED
SUM(x * hours) <= max_hours PER empID
```

Where `max_hours` varies per group. Requires resolving which row's value to use per group (e.g., validate all rows in a group have the same value, or take the first).
