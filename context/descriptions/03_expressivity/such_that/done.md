# SUCH THAT Clause — Implemented Features

The `SUCH THAT` clause specifies the **constraints** of a DECIQL query. The solver only accepts variable assignments that satisfy all constraints.

---

## Syntax

```sql
SUCH THAT
    constraint_expression
    [AND constraint_expression2 ...]
```

Multiple constraints are separated by `AND`. Each constraint evaluates to a boolean.

---

## Constraint Types

### Simple Comparison Constraints

Any comparison involving at least one decision variable (or aggregate over one).

**Supported operators**: `=`, `<>`, `<`, `<=`, `>`, `>=`

```sql
SUCH THAT
    x <= 1                      -- per-row: each x is at most 1
    SUM(x * weight) <= 50       -- aggregate: total weight under budget
    SUM(x) >= 10                -- at least 10 items selected
```

### BETWEEN

`expr BETWEEN a AND b` desugars to `expr >= a AND expr <= b`. Both bounds become separate constraints.

```sql
SUCH THAT
    SUM(keep) BETWEEN 200 AND 400      -- between 200 and 400 items selected
```

### IN

`x IN (val1, val2, ...)` constrains a value to be in the provided set.

```sql
SUCH THAT
    category IN ('A', 'B', 'C')    -- row-level filter on a column
```

---

## AND Separator

Constraints are joined by `AND`. Each `AND`-separated expression is a distinct constraint in the optimization model.

> **Important**: `AND` inside a `WHEN` condition has different meaning — it is part of the condition, not a constraint separator. Use parentheses: `SUM(x * w) <= 50 WHEN (cat = 'A' AND status = 'active')`.

---

## Aggregate vs. Per-Row Constraints

**Aggregate constraints** use `SUM(...)` over the relation (or filtered subset). These compile to a single linear inequality in the ILP.

```sql
SUM(x * weight) <= 50                          -- one constraint over all rows
SUM(x * weight) <= 20 WHEN category = 'A'      -- one constraint over category-A rows
```

**Per-row constraints** do not use an aggregate. The system generates one constraint per input row.

```sql
x <= 1                          -- one constraint generated per row
```

---

## Linearity Requirement

All expressions involving decision variables must be linear:

| Expression | Status |
|---|---|
| `x * 5` — variable times literal | OK |
| `x * column` — variable times table column | OK |
| `SUM(x * column)` | OK |
| `x + y` | OK |
| `x * y` — two variables multiplied | **ERROR: non-linear** |

---

## Subqueries in Constraints

### Uncorrelated Scalar Subqueries

A subquery that returns a single value and does not reference the outer query's table is allowed. Evaluated once and treated as a constant.

```sql
SUCH THAT
    x <= (SELECT avg(budget) FROM Depts)
```

---

## WHEN (Conditional Constraints)

Constraints can be made conditional using the postfix `WHEN` keyword. See [when/done.md](../when/done.md) for full details.

```sql
SUCH THAT
    SUM(x * weight) <= 50 WHEN category = 'electronics' AND
    SUM(x * weight) <= 30 WHEN category = 'clothing'
```

---

## Examples

```sql
-- Knapsack with two resource limits
SUCH THAT
    SUM(x * weight) <= 50 AND
    SUM(x * volume) <= 30

-- Conditional weight limits per category
SUCH THAT
    SUM(x * weight) <= 50 WHEN category = 'electronics' AND
    SUM(x * weight) <= 30 WHEN category = 'clothing' AND
    x <= 1
```

---

## Code Pointers

- **Constraint binder**: `src/planner/expression_binder/decide_constraints_binder.cpp`
  - `BindComparison()` — handles `=`, `<>`, `<`, `<=`, `>`, `>=`
  - `BindBetween()` (lines 216-232) — desugars to two comparison constraints
  - `BindOperator()` (lines 198-210) — handles IN clause
  - `BindWhenConstraint()` (lines 258-302) — handles WHEN modifier
  - Lines 358-367: Validates that only SUM is used as aggregate function

- **Execution** (constraint matrix construction): `src/execution/operator/decide/physical_decide.cpp`
