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

### =, <>, <, <=, >, >=

All six standard comparison operators are supported in constraints.

```sql
SUCH THAT x <= 1
SUCH THAT SUM(x * w) >= 10
SUCH THAT SUM(x) = 5
```

### BETWEEN ... AND ...

Desugars to `>= lower AND <= upper`. Produces two constraints.

```sql
SUCH THAT SUM(x) BETWEEN 10 AND 50
-- equivalent to: SUM(x) >= 10 AND SUM(x) <= 50
```

### IN (...)

Constrains a value to be in a literal set.

```sql
SUCH THAT category IN ('A', 'B', 'C')
```

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
| `*` (var x const/col) | Yes | Yes | N/A |
| `+`, `-` | Yes | Yes | Yes |
| `=`, `<>`, `<`, `<=`, `>`, `>=` | Yes | N/A | Yes |
| `BETWEEN` | Yes | N/A | Yes |
| `IN (...)` | Yes | N/A | Yes |
| `IS NULL` / `IS NOT NULL` | N/A | N/A | Yes |
| `AND` (constraint sep.) | Yes | N/A | N/A |
| `AND` / `OR` (logical) | N/A | N/A | Yes |
