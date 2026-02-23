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
-- NOT YET IMPLEMENTED
SUCH THAT
    SUM(new_hours) <= 40 PER empID
```

---

## Syntax

```sql
-- Constraint
constraint_expression [WHEN condition] PER column [, column2 ...]

-- Objective
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

## Use Cases

### Per-Employee / Per-Group Resource Limits (Repair)

```sql
-- NOT YET IMPLEMENTED
SELECT * DECIDE new_hours IS INTEGER
FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
SUCH THAT
    SUM(new_hours) <= 40 PER P.empID AND
    SUM(new_hours) <= 30 WHEN E.title = 'Director' PER P.empID
MINIMIZE SUM(ABS(new_hours - hours)) PER projectID
```

### Per-Label Coverage (Active Learning / Selection)

```sql
-- NOT YET IMPLEMENTED
SELECT * DECIDE keep IS BOOLEAN FROM Reviews
SUCH THAT
    SUM(keep * weak_label) >= 50 PER weak_label AND
    SUM(keep) BETWEEN 200 AND 400
MINIMIZE SUM(keep * confidence)
```

Here `PER weak_label` generates one coverage constraint per distinct label class, ensuring adequate representation.

### Per-Zipcode Aggregate Matching (Synthesis)

```sql
-- NOT YET IMPLEMENTED (also requires IS REAL)
SELECT hash(tid) AS syn_tid
DECIDE syn_beds IS INTEGER, syn_rent IS REAL FROM rentals
SUCH THAT
    syn_beds >= 0 AND syn_rent >= 0 AND
    AVG(syn_rent - rent) = 0 PER zipcode AND
    AVG(syn_beds - beds) = 0 PER zipcode
MINIMIZE SUM(ABS(syn_rent - (alpha + beta * syn_beds)))
```

---

## Implementation Plan

### Step 1: Grammar

Add `PER` as a postfix modifier in the grammar, similar to how WHEN is handled:

```
decide_constraint_item:
    a_expr                              -- no modifier
    | a_expr WHEN a_expr                -- WHEN only
    | a_expr PER column_list            -- PER only
    | a_expr WHEN a_expr PER column_list -- WHEN + PER
```

This requires adding `PER` to the keyword list and extending `decide_constraint_item` and `decide_objective_item` rules in `select.y`.

### Step 2: Binder

In `decide_constraints_binder.cpp`:
1. Detect the `PER` modifier on a constraint (similar to how WHEN is detected via tag)
2. Store the PER column reference(s) alongside the constraint expression
3. Validate that PER columns reference only table columns (not decision variables)

### Step 3: Execution

In `physical_decide.cpp`, before constructing the ILP matrix:
1. For each PER-annotated constraint, scan the (optionally WHEN-filtered) input to collect distinct values of the PER column(s)
2. For each distinct value, generate a copy of the constraint with an implicit `WHEN per_col = value` filter
3. Add all generated constraints to the ILP matrix

### Step 4: Scaling Considerations

The number of generated constraints equals `|distinct_values| x |PER_constraints|`. For large relations this can produce O(|D|) constraints (one per tuple in the worst case). This is a key motivation for the optimizer's constraint reduction strategies (see [../../04_optimizer/problem_reduction/todo.md](../../04_optimizer/problem_reduction/todo.md)).

**Mitigation strategies** (future optimizer work):
- **Constraint-to-bound conversion**: Detect PER constraints that are equivalent to simple variable bounds
- **Skyband pruning**: Eliminate dominated tuples before generating constraints
- **Drop-solve-validate-refine loop**: Generate a subset of constraints, solve, check if dropped constraints are violated, and iteratively refine
