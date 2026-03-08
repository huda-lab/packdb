# SQL Functions & Expressions — Implemented Features

This file documents which SQL functions and expressions work inside DECIQL clauses today.

---

## Aggregate Functions

### SUM()

The only supported aggregate over expressions involving decision variables. Valid in both constraints and objectives.

```sql
MAXIMIZE SUM(x * value)
SUCH THAT SUM(x * weight) <= 50
```

**Code**: Validated in `decide_objective_binder.cpp:91-100` and `decide_constraints_binder.cpp:358-367` — any aggregate other than SUM is rejected with an error.

### COUNT() — BOOLEAN and INTEGER variables

`COUNT(x)` counts the number of rows where the decision variable `x` is non-zero. It is supported for both BOOLEAN and INTEGER variables (REAL is not yet supported).

**BOOLEAN variables**: `COUNT(x)` is rewritten to `SUM(x)` directly, since for a {0,1} variable, "how many rows have x=1" equals `SUM(x)`.

**INTEGER variables**: `COUNT(x)` uses the Big-M indicator variable technique:
1. A hidden binary indicator variable `z` is introduced for `x`
2. Two linking constraints enforce the relationship: `z <= x` (z=0 when x=0) and `x <= M*z` (z=1 when x>0)
3. `COUNT(x)` is rewritten to `SUM(z)`, which counts non-zero assignments
4. M is derived from the upper bound of `x` (defaults to 1e6 if no bound specified)

```sql
SUCH THAT COUNT(x) >= 5     -- where x IS BOOLEAN or INTEGER
MAXIMIZE COUNT(x)           -- maximize number of non-zero rows
SUCH THAT COUNT(x) <= 2     -- at most 2 non-zero assignments
```

The rewrite happens early, before normalization and binding, in `bind_select_node.cpp` via the `RewriteCountToSum()` function. This means COUNT inherits all SUM capabilities (WHEN, PER, all comparison operators) for free. Multiple `COUNT(x)` references to the same variable reuse a single indicator.

`COUNT(x)` is **rejected** for REAL variables with a clear error message.

---

### AVG() — Rewritten to SUM with RHS Scaling

`AVG(expr)` over decision variables is rewritten to `SUM(expr)` early in the bind phase, with an alias tag (`__avg_rewrite__`) that signals the model builder to scale the constraint RHS by the row count N.

**Semantics**: Standard SQL AVG — divide by count of all rows (decision variables are never NULL). This is always linear since N is a data-determined constant.

**Constraints**: `AVG(expr) op K` becomes `SUM(expr) op K*N` where N depends on context:
- No WHEN/PER: N = total row count
- WHEN: N = count of WHEN-matching rows
- PER: N = count of rows in each group
- WHEN+PER: N = count of WHEN-matching rows per group

**Objectives**: `MAXIMIZE/MINIMIZE AVG(expr)` simply becomes `SUM(expr)` — same argmax/argmin since N > 0 is constant.

```sql
SUCH THAT AVG(x * weight) <= 10         -- SUM(x*weight) <= 10*N
SUCH THAT AVG(x) <= 0.5                 -- at most half the rows selected (BOOLEAN)
SUCH THAT AVG(x * cost) <= 5 WHEN active -- only among active rows
SUCH THAT AVG(x * hours) <= 8 PER emp   -- per-group average
MAXIMIZE AVG(x * profit)                -- same as MAXIMIZE SUM(x * profit)
```

**Code**: AVG flows through binding natively (no parse-time rewrite), preserving its DOUBLE return type so fractional RHS values survive type coercion. The binders (`decide_constraints_binder.cpp`, `decide_objective_binder.cpp`) accept `"avg"` alongside `"sum"`. At execution time (`physical_decide.cpp`), `BoundAggregateExpression::function.name == "avg"` sets `LinearConstraint::was_avg_rewrite`, propagated to `EvaluatedConstraint::was_avg_rewrite`. RHS scaling applied in `ilp_model_builder.cpp` at model build time.

**Tests**: `test/decide/tests/test_avg.py` — 9 test cases covering objectives, constraints, WHEN, PER, WHEN+PER, BOOLEAN, INTEGER, non-linear rejection, and no-decide-var passthrough.

---

## ABS() — Linearized Automatically

`ABS(expr)` over decision variables is automatically linearized using the standard ILP technique. For each `ABS(expr)` that references a DECIDE variable, the system:

1. Introduces an auxiliary REAL variable `d` (hidden from query output)
2. Adds two constraints: `d >= expr` and `d >= -expr`
3. Replaces `ABS(expr)` with `d`

This works in both constraints and objectives:

```sql
-- In objectives: minimize total absolute deviation
MINIMIZE SUM(ABS(new_hours - hours))

-- In per-row constraints: bound deviation per row
SUCH THAT ABS(new_qty - l_quantity) <= 5

-- In aggregate constraints: bound total deviation
SUCH THAT SUM(ABS(new_qty - l_quantity)) <= 50
```

`ABS()` without decision variables (e.g., `ABS(col1 - col2)`) is left as regular SQL — no rewrite occurs.

**Code**: The rewrite happens early in `bind_select_node.cpp` via `RewriteAbsLinearization()`, before normalization and binding. Auxiliary variables are included in the DECIDE variable pipeline but hidden from `SELECT *` by truncating the bind context after binding.

**Tests**: `test/decide/tests/test_abs_linearization.py` — 8 test cases covering objectives, constraints, WHEN, PER, multiple ABS terms, no-decide-var, and mixed variable types.

---

## Arithmetic Operators

### Multiplication (`*`) — variable x constant or variable x column

```sql
x * 5              -- OK: variable * literal
x * weight         -- OK: variable * column (constant per row)
SUM(x * weight)    -- OK: aggregate of linear product
```

`x * y` (variable x variable) is **not supported** (non-linear).

### Addition / Subtraction (`+`, `-`)

```sql
x + y              -- OK: sum of variables
SUM(x * a + y * b) -- OK: linear combination in aggregate
```

---

## Comparison Operators

### =, <, <=, >, >=

Five standard comparison operators are supported in constraints.

```sql
SUCH THAT x <= 1
SUCH THAT SUM(x * w) >= 10
SUCH THAT SUM(x) = 5
```

> **Note**: `<>` (not-equal) is parsed but **rejected by the binder on aggregates** (e.g., `SUM(x) <> 5`). It creates a disjunctive constraint that requires Big-M reformulation. See [../../04_optimizer/query_rewriting/todo.md](../../04_optimizer/query_rewriting/todo.md).

### BETWEEN ... AND ...

Desugars to `>= lower AND <= upper`. Produces two constraints.

```sql
SUCH THAT SUM(x) BETWEEN 10 AND 50
-- equivalent to: SUM(x) >= 10 AND SUM(x) <= 50
```

### IN (...)

Constrains a **table column** value to be in a literal set.

```sql
SUCH THAT category IN ('A', 'B', 'C')
```

> **Limitation**: `IN` on **decision variables** (e.g., `x IN (0, 1, 3)`) is parsed and bound but does not enforce the domain restriction at the solver level. Proper support requires auxiliary binary variables.

---

## NULL-Related Expressions

### IS NULL / IS NOT NULL

Supported in WHEN conditions and WHERE clause (not over decision variables).

```sql
imputed_distance = distance WHEN distance IS NOT NULL AND
imputed_distance <= 10 WHEN (distance IS NULL AND mode = 'walk-bike')
```

---

## Boolean / Logical Operators

### AND — Two Roles

1. **Constraint separator** at the top level of `SUCH THAT`
2. **Logical AND** inside a `WHEN` condition (requires parentheses)

### OR

Valid in `WHEN` conditions and `WHERE` only. Not supported as a constraint combiner over decision variables (would create non-linear disjunctions).

---

## Summary Table (Implemented Only)

| Function / Operator | In Constraints | In Objective | In WHEN / WHERE |
|---|---|---|---|
| `SUM()` over dec. vars | Yes | Yes | N/A |
| `AVG()` over dec. vars | Yes (RHS scaled) | Yes (→SUM) | N/A |
| `COUNT()` (BOOLEAN, INTEGER) | Yes | Yes | N/A |
| `ABS()` over dec. vars | Yes (linearized) | Yes (linearized) | N/A |
| `*` (var x const/col) | Yes | Yes | N/A |
| `+`, `-` | Yes | Yes | Yes |
| `=`, `<`, `<=`, `>`, `>=` | Yes | N/A | Yes |
| `<>` (not-equal) | Rejected on aggregates | N/A | Yes |
| `BETWEEN` | Yes | N/A | Yes |
| `IN (...)` | Table columns only | N/A | Yes |
| `IS NULL` / `IS NOT NULL` | N/A | N/A | Yes |
| `AND` (constraint sep.) | Yes | N/A | N/A |
| `AND` / `OR` (logical) | N/A | N/A | Yes |
