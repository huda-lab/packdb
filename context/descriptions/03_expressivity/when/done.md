# WHEN Keyword — Implemented Features

`WHEN` is a postfix conditional modifier applied to both constraints (in `SUCH THAT`) and objectives (`MAXIMIZE`/`MINIMIZE`). It causes the expression to apply only to rows where the condition is true.

---

## Syntax

```sql
-- On a constraint
constraint_expression WHEN condition

-- On an objective
MAXIMIZE SUM(...) WHEN condition
MINIMIZE SUM(...) WHEN condition
```

`WHEN` always appears *after* the main expression and *before* the `AND` separator.

---

## Semantics

### WHEN on Aggregate Constraints

For aggregate constraints (those using `SUM`), `WHEN` filters which rows are included in the aggregate. Rows where the condition is false (or NULL) are excluded. The solver sees a standard linear inequality with coefficients zeroed out for non-matching rows.

```sql
SUM(x * weight) <= 50 WHEN category = 'electronics'
```

Equivalent formulation (for illustration):
```sql
SUM(CASE WHEN category = 'electronics' THEN x * weight ELSE 0 END) <= 50
```

### WHEN on Per-Row Constraints

For constraints without an aggregate, `WHEN` causes the constraint to be *skipped entirely* for non-matching rows. No constraint is generated for those rows in the ILP.

```sql
x <= 1 WHEN status = 'active'    -- no constraint generated for inactive rows
```

### WHEN on Objective

For the objective, `WHEN` zeros out the contribution of non-matching rows. The solver sees a standard dense objective vector with zeros for excluded rows.

```sql
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'
```

---

## NULL Handling

If the `WHEN` condition evaluates to NULL for a row, that row is treated as **not matching** (same as false).

---

## Rules and Restrictions

### Conditions Must Reference Only Table Columns

`WHEN` conditions may only reference columns from the input relation. Decision variables are **not allowed** in `WHEN` conditions.

```sql
-- OK
SUM(x * weight) <= 50 WHEN category = 'electronics'

-- ERROR: decision variable in WHEN condition
SUM(x * weight) <= 50 WHEN x = 1
```

**Reason**: `WHEN` conditions are evaluated before the solver runs, to construct the coefficient matrix.

### Compound Conditions Require Parentheses

When a `WHEN` condition uses `AND` or `OR`, it must be wrapped in parentheses to avoid ambiguity with the `AND` constraint separator.

```sql
-- Correct
SUM(x * weight) <= 20 WHEN (category = 'A' AND status = 'active')

-- Incorrect: the parser treats AND as constraint separator
SUM(x * weight) <= 20 WHEN category = 'A' AND status = 'active'
```

### Expressions Without WHEN Apply Unconditionally

A constraint or objective without `WHEN` applies to all rows.

---

## Examples

```sql
-- Constraint: different weight limits per category
SUCH THAT
    SUM(x * weight) <= 50 WHEN category = 'electronics' AND
    SUM(x * weight) <= 30 WHEN category = 'clothing' AND
    x <= 1

-- Constraint: compound condition (parentheses required)
SUCH THAT
    SUM(x * weight) <= 20 WHEN (category = 'A' AND status = 'active')

-- Objective: only count electronics toward the objective
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'

-- Constraint + objective both using WHEN
SUCH THAT
    SUM(x * weight) <= 100 WHEN region = 'US'
MAXIMIZE SUM(x * value) WHEN region = 'US'
```

---

## Note: WHEN vs SQL CASE WHEN

PackDB's `WHEN` is a **row filter** — it controls whether a constraint or objective *applies* to a row. SQL's `CASE WHEN` is a **value expression** — it produces different values conditionally. These serve different purposes and are not interchangeable.

When you need **conditional coefficients or bounds** (different values per row based on conditions), use a CTE or subquery to pre-compute the value, then reference the resulting column inside DECIDE. This avoids any need to support `CASE WHEN` within the DECIDE clause itself.

### Example 1: Conditional Penalty Weights in Objective

Suppose director hour changes should be penalized 3x and manager changes 2x:

```sql
WITH weighted AS (
  SELECT *,
    CASE WHEN title = 'Director' THEN 3
         WHEN title = 'Manager'  THEN 2
         ELSE 1 END AS penalty_weight
  FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
)
SELECT * DECIDE new_hours IS INTEGER
FROM weighted
SUCH THAT ...
MINIMIZE SUM(penalty_weight * abs(new_hours - hours))
```

`penalty_weight` is a table column by the time DECIDE sees it, so it works as a standard coefficient.

### Example 2: Conditional Effectiveness in Constraint

Suppose director hours count as twice as effective:

```sql
WITH effective AS (
  SELECT *,
    CASE WHEN title = 'Director' THEN 2 ELSE 1 END AS effectiveness
  FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
)
SELECT * DECIDE new_hours IS INTEGER
FROM effective
SUCH THAT
  SUM(new_hours * effectiveness) >= 60 PER projectID
```

Note: this is **not equivalent** to decomposing into separate WHEN constraints (`SUM(new_hours) >= 60 PER projectID AND SUM(new_hours) >= 30 WHEN title='Director' PER projectID`), which creates two independent constraints rather than one combined weighted sum.

### Example 3: Conditional Bounds with Overlapping Conditions

Suppose rent tolerance depends on zipcode and bedroom count, with overlapping conditions:

```sql
WITH toleranced AS (
  SELECT *,
    CASE WHEN zipcode = 10003 THEN 500
         WHEN beds >= 3       THEN 300
         ELSE 150 END AS tolerance
  FROM rentals
)
SELECT * DECIDE syn_rent IS INTEGER
FROM toleranced
SUCH THAT
  abs(syn_rent - rent) <= tolerance
```

Decomposing into multiple WHEN constraints fails here because the conditions overlap (e.g., a 3-bed apartment in zipcode 10003). SQL `CASE WHEN` evaluates top-to-bottom and returns the first match, giving the correct priority semantics. Pre-computing it as a column preserves that behavior.

### Summary

| Need | Use |
|------|-----|
| Include/exclude rows from a constraint or objective | `WHEN` (postfix) |
| Different coefficient values per row | `CASE WHEN` in a CTE, referenced as a column |

---

## Code Pointers

- **Grammar**: `third_party/libpg_query/grammar/statements/select.y`
  - Lines 211-214: `decide_objective_item` rule with WHEN support
  - Lines 250-254: `decide_constraint_item` rule with WHEN support

- **Constraint binder**: `src/planner/expression_binder/decide_constraints_binder.cpp`
  - `BindWhenConstraint()` (lines 258-302): Extracts the WHEN condition as a separate boolean expression. Validates that the condition references only table columns, not decision variables.

- **Objective binder**: `src/planner/expression_binder/decide_objective_binder.cpp`
  - Lines 29-66: Handles WHEN condition extraction on the objective expression.

- **Execution**: `src/execution/operator/decide/physical_decide.cpp`
  - Lines 200-203: WHEN condition stored with `WHEN_CONSTRAINT_TAG` alias
  - Lines 263-282: Per-row WHEN mask evaluation — boolean mask created from the condition, applied to coefficient arrays before constructing the ILP matrix

- **Tag constant**: `src/include/duckdb/common/enums/decide.hpp:31`
  ```cpp
  WHEN_CONSTRAINT_TAG = "__when_constraint__"
  ```
