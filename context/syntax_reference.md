# Syntax Reference

## 1. The DECIDE Clause usage

```sql
SELECT ...
DECIDE variable_name [, variable_name2 ...]
SUCH THAT
    constraint_expression
    [, constraint_expression2 ...]
[MAXIMIZE | MINIMIZE] objective_expression
```

## 2. Decision Variables
-   Must be declared in `DECIDE` list.
-   Scope: Available in `SUCH THAT`, `MAXIMIZE/MINIMIZE`, and the `SELECT` list.
-   **Syntactic Is-Sugar**:
    -   `x IS INTEGER`: Default. $x \in \{0, 1, 2, ...\}$
    -   `x IS BINARY`: $x \in \{0, 1\}$
    -   `x IS REAL`: **Not Supported** (Error).

## 3. Constraints
Constraints must evaluate to a boolean.
-   **Supported Operators**: `=`, `<>`, `<`, `<=`, `>`, `>=`.
-   **Between**: `expr BETWEEN a AND b` $\rightarrow$ `expr >= a AND expr <= b`.
-   **In**: `x IN (1, 2, 3)`.
-   **Linearity**: Any sub-expression involving a decision variable must be linear.
    -   `x * 5`: OK.
    -   `x + y`: OK.
    -   `x * column`: OK (column is constant per row).
    -   `x * y`: **ERROR** (Non-linear).

## 4. Objective
-   Must be a single aggregate expression: `SUM(...)`.
-   Must involve at least one decision variable.
-   Must be linear.

## 5. Aggregations
-   Only `SUM()` is supported over decision variables.
-   `COUNT(x)` is **Not Supported** (Use `SUM(x)` where x is binary).
-   `AVG(x)` is **Not Supported** (Non-linear ratio).
