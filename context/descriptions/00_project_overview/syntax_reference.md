# Syntax Reference

## 1. The DECIDE Clause usage

```sql
SELECT ...
DECIDE variable_name [IS type] [, variable_name2 [IS type2] ...]
SUCH THAT
    constraint_expression
    [, constraint_expression2 ...]
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

Constraints must evaluate to a boolean.

- **Supported Operators**: `=`, `<>`, `<`, `<=`, `>`, `>=`.
- **Between**: `expr BETWEEN a AND b` $\rightarrow$ `expr >= a AND expr <= b`.
- **In**: `x IN (1, 2, 3)`.
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
- `COUNT(x)` is **Not Supported** (Use `SUM(x)` where x is boolean).
- `AVG(x)` is **Not Supported** (Non-linear ratio).
