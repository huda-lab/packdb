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
  - `x IS REAL`: $x \in [0, \infty)$ (continuous, non-negative)

**Examples:**

```sql
DECIDE x IS BOOLEAN           -- x is binary (0 or 1)
DECIDE x IS INTEGER           -- x is non-negative integer
DECIDE x                      -- same as x IS INTEGER (default)
DECIDE x IS REAL              -- x is continuous, non-negative
DECIDE x IS BOOLEAN, y IS INTEGER, z IS REAL  -- multiple typed variables
```

## 3. Constraints

Constraints must evaluate to a boolean. Multiple constraints are separated by `AND`.

- **Supported Operators**: `=`, `<`, `<=`, `>`, `>=`, `<>`.
  - `<>` (not-equal): Supported on both per-row and aggregate constraints via Big-M disjunction (1 auxiliary binary variable + 2 constraints per `<>`).
- **Between**: `expr BETWEEN a AND b` $\rightarrow$ `expr >= a AND expr <= b`.
- **In**: `x IN (v1, ..., vK)` — works on both table columns and decision variables. On decision variables, rewritten to K binary indicator variables with cardinality + linking constraints. `IN` on aggregates (e.g., `SUM(x) IN (...)`) is not supported.
- **Linearity**: Any sub-expression involving a decision variable must be linear.
  - `x * 5`: OK.
  - `x + y`: OK.
  - `x * column`: OK (column is constant per row).
  - `x * y`: **ERROR** (Non-linear).
- **Subqueries**: Scalar subqueries (both uncorrelated and correlated) are allowed on the RHS of constraints. Correlated subqueries are decorrelated into joins, producing per-row values. For aggregate constraints, the subquery RHS must evaluate to the same scalar for all rows. Subqueries cannot reference DECIDE variables.

## 4. Objective

- Must be a single aggregate expression: `SUM(...)`, `AVG(...)`, `MIN(...)`, or `MAX(...)`.
- Must involve at least one decision variable.
- Linear objectives: must be linear in decision variables.
- **Quadratic objectives (QP)**: `MINIMIZE SUM(POWER(linear_expr, 2))` is supported for convex quadratic programming. The inner expression must be linear in decision variables. Three equivalent syntax forms:
  - `POWER(expr, 2)` / `POW(expr, 2)` — function call
  - `expr ** 2` — exponentiation operator
  - `(expr) * (expr)` — identical multiplication (both sides must be the same expression)
  - Only `MINIMIZE` is allowed (maximizing a sum of squares is non-convex).
  - Gurobi supports both continuous QP and mixed-integer QP (MIQP). HiGHS supports continuous QP only — integer/boolean variables with quadratic objectives require Gurobi.
  - Quadratic constraints (QCQP) are not yet supported.

## 5. Aggregations

- `SUM()` is the primary aggregate over decision variables.
- `COUNT(x)` is supported for **BOOLEAN and INTEGER variables**. BOOLEAN is rewritten to `SUM(x)`; INTEGER uses a Big-M indicator variable rewrite.
- `AVG(expr)` is supported. Rewritten to `SUM(expr)` with RHS scaled by row count N at execution time. For objectives, `AVG` and `SUM` share the same argmax/argmin. For constraints, `AVG(expr) op K` becomes `SUM(expr) op K*N` where N is the row count (adjusted for WHEN/PER context).
- `MIN(expr)` and `MAX(expr)` are supported. Easy cases (`MAX(expr) <= K`, `MIN(expr) >= K`) become per-row constraints with no auxiliary variables. Hard cases (opposite direction, equality) use a global auxiliary variable and Big-M binary indicators. In objectives, `MINIMIZE MAX(expr)` and `MAXIMIZE MIN(expr)` use a global auxiliary; `MAXIMIZE MAX(expr)` and `MINIMIZE MIN(expr)` additionally require Big-M indicators. Composes with WHEN.

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

The `PER` keyword generates one constraint per distinct value (or combination of values) of column(s).

**Syntax**: `SUM(expr) comparison rhs PER column` or `PER (col1, col2, ...)`

```sql
SUCH THAT
    SUM(x * hours) <= 40 PER empID
    SUM(x * hours) <= 40 PER (empID, department)
```

### 7.1 PER + WHEN Composition

WHEN filters rows first, then PER groups the remaining rows:

```sql
SUCH THAT
    SUM(x * hours) <= 30 WHEN title = 'Director' PER empID
    SUM(x * hours) <= 30 WHEN title = 'Director' PER (empID, department)
```

### 7.2 PER on Objective — Nested Aggregates

PER on objectives uses nested aggregate syntax to specify both per-group and across-group aggregation:

```sql
-- Nested aggregate: OUTER(INNER(expr)) PER col
MINIMIZE SUM(MAX(x * cost)) PER department
MAXIMIZE MIN(SUM(x * profit)) PER region
MINIMIZE MAX(SUM(x * hours)) PER empID
```

All 9 combinations of `SUM`/`MIN`/`MAX` for outer and inner aggregates are supported.

**Flat aggregate + PER**:
- `SUM(expr) PER col` or `AVG(expr) PER col`: Accepted (no-op — global sum equals sum of group sums).
- `MIN(expr) PER col` or `MAX(expr) PER col` (flat): **Error** — use nested form instead.

WHEN + PER composition is supported: `MINIMIZE MAX(SUM(x * hours)) WHEN active = 1 PER empID`.

### 7.3 Restrictions

- **Aggregate-only**: PER requires a SUM constraint (per-row constraints are rejected).
- **Column references only**: Each PER column must be a simple column reference, not an expression or decision variable.
- **Constant RHS**: The right-hand side must be constant across groups.
- **NULL handling**: Rows where any PER column is NULL are excluded.
