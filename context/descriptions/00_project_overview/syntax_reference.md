# Syntax Reference

## 1. The DECIDE Clause usage

```sql
SELECT ...
DECIDE variable_name [IS type] [, variable_name2 [IS type2] ...]
SUCH THAT
    constraint_expression
    [AND constraint_expression2 ...]
[MAXIMIZE | MINIMIZE] objective_expression
```

## 2. Decision Variables

- Must be declared in `DECIDE` list with optional type annotation.
- Scope: Available in `SUCH THAT`, `MAXIMIZE/MINIMIZE`, and the `SELECT` list.
- **Type Declarations** (in DECIDE clause):
  - `x IS INTEGER`: Default if no type specified. $x \in \{0, 1, 2, ...\}$
  - `x IS BOOLEAN`: $x \in \{0, 1\}$ (automatically adds bounds constraints)
  - `x IS REAL`: **Not Supported** (Error).

**Examples:**

```sql
DECIDE x IS BOOLEAN           -- x is binary (0 or 1)
DECIDE x IS INTEGER           -- x is non-negative integer
DECIDE x                      -- same as x IS INTEGER (default)
DECIDE x IS BOOLEAN, y IS INTEGER  -- multiple typed variables
```

## 3. Constraints

Constraints must evaluate to a boolean. Multiple constraints are separated by `AND`.

- **Supported Operators**: `=`, `<`, `<=`, `>`, `>=`.
  - `<>` (not-equal): Parsed but **rejected on aggregates** (requires Big-M reformulation).
- **Between**: `expr BETWEEN a AND b` $\rightarrow$ `expr >= a AND expr <= b`.
- **In**: `column IN (1, 2, 3)` — works on table columns. On decision variables, parsed but **not enforced** (requires auxiliary variables).
- **Linearity**: Any sub-expression involving a decision variable must be linear.
  - `x * 5`: OK.
  - `x + y`: OK.
  - `x * column`: OK (column is constant per row).
  - `x * y`: **ERROR** (Non-linear).

## 4. Objective

- Must be a single aggregate expression: `SUM(...)`.
- Must involve at least one decision variable.
- Must be linear.

## 5. Aggregations

- Only `SUM()` is supported over decision variables.
- `COUNT(x)` is supported for **BOOLEAN variables only** (automatically rewritten to `SUM(x)`).
- `AVG(x)` is **Not Supported** (Non-linear ratio).

## 6. Conditional Expressions — `WHEN`

The `WHEN` keyword enables conditional constraints and conditional objectives. A `WHEN` clause causes the expression to apply only to rows where the condition evaluates to true. Rows where the condition is false or NULL are excluded.

**Syntax**: `expression WHEN condition`

### 6.1 WHEN on Constraints

```sql
SUCH THAT
    SUM(x * weight) <= 50 WHEN category = 'electronics' AND
    SUM(x * weight) <= 30 WHEN category = 'clothing' AND
    x <= 1
```

**Execution**: WHEN conditions create per-row boolean masks. For aggregate constraints (`SUM`), masked-out rows are excluded from the summation. For per-row constraints, masked-out rows skip constraint generation entirely.

### 6.2 WHEN on Objective

```sql
MAXIMIZE SUM(x * profit) WHEN category = 'electronics'
MINIMIZE SUM(x * cost) WHEN region = 'US'
```

**Execution**: Objective coefficients for non-matching rows are zeroed out. The solver sees a standard ILP where only matching rows contribute to the objective value.

### 6.3 Rules (applies to both constraints and objectives)

- `WHEN` is postfix: the expression comes first, then `WHEN condition`.
- The `WHEN` condition must reference only table columns, **not** decision variables.
- NULL conditions are treated as false (expression does not apply for that row).
- When a `WHEN` condition contains `AND`/`OR`, parentheses are required:
  ```sql
  SUM(x * weight) <= 20 WHEN (category = 'A' AND status = 'active')
  ```
- Expressions without `WHEN` apply unconditionally to all rows.

## 7. Group-Scoped Constraints — `PER`

The `PER` keyword generates one constraint per distinct value of a column.

**Syntax**: `SUM(expr) comparison rhs PER column`

```sql
SUCH THAT
    SUM(x * hours) <= 40 PER empID
```

### 7.1 PER + WHEN Composition

WHEN filters rows first, then PER groups the remaining rows:

```sql
SUCH THAT
    SUM(x * hours) <= 30 WHEN title = 'Director' PER empID
```

### 7.2 Restrictions

- **Aggregate-only**: PER requires a SUM constraint (per-row constraints are rejected).
- **Single-column**: Only `PER column` (not `PER (col1, col2)`).
- **Constant RHS**: The right-hand side must be constant across groups.
- **Table columns only**: PER column must be a table column, not a decision variable.
- **NULL handling**: Rows where the PER column is NULL are excluded.
