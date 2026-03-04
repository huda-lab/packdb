# SQL Functions & Expressions — Planned Features

---

## ABS()

**Priority: High** (needed for repair and imputation objectives)

`ABS(expr)` is needed for minimizing absolute deviations — the core objective for data repair tasks.

```sql
-- NOT YET IMPLEMENTED
MINIMIZE SUM(ABS(new_hours - hours))
```

### Linearization Approach

`ABS(expr)` is non-linear but can be linearized with standard ILP technique:

1. Introduce an auxiliary REAL variable `d_i` for each row `i`
2. Add two constraints per row: `d_i >= expr_i` and `d_i >= -expr_i`
3. Replace `ABS(expr_i)` with `d_i` in the objective

This transformation should be applied automatically during query planning (see [../../04_optimizer/query_rewriting/todo.md](../../04_optimizer/query_rewriting/todo.md) for Big-M reformulation).

### Dependencies

- ~~Requires `IS REAL` variable support~~ — **done** (see [../decide/done.md](../decide/done.md))
- Requires optimizer support for introducing auxiliary variables

---

## COUNT() Over INTEGER/REAL Decision Variables

**Priority: Low**

`COUNT(x)` for BOOLEAN variables is implemented (see `done.md`). For INTEGER and REAL variables, COUNT semantics are ambiguous — does it mean "count of non-zero values"? "count of rows"? This is currently rejected with a clear error message.

If needed, a possible approach would be to introduce an auxiliary BOOLEAN variable `y_i` with constraints `y_i <= x_i` and `y_i * M >= x_i` (Big-M), then rewrite `COUNT(x)` to `SUM(y)`. This requires Big-M infrastructure.

---

## AVG() Over Decision Variables

**Priority: Medium**

`AVG(x * col)` expands to `SUM(x * col) / COUNT(*)`, which is a non-linear ratio. However, it **can** be linearized when the denominator is a known constant:

```sql
-- NOT SUPPORTED (currently)
SUCH THAT AVG(x * weight) <= 10

-- Linearized form (multiply both sides by N, the row count):
SUCH THAT SUM(x * weight) <= 10 * N
```

### Suggested Implementation

1. Detect `AVG(expr)` in constraints/objectives where `expr` involves decision variables
2. Determine the row count `N` (from the input relation, after WHERE/WHEN filtering)
3. Rewrite: `AVG(expr) <= K` becomes `SUM(expr) <= K * N`
4. For `AVG(expr) = K`: becomes `SUM(expr) = K * N`

**Caveat**: When combined with `WHEN`, `N` is the count of WHEN-matching rows, which may not be known at bind time. This may require deferred rewriting at execution time.

**Caveat**: When `x IS BOOLEAN`, `AVG(x * col)` means "average of `col` among selected rows" — the denominator is `SUM(x)` (unknown), not `N`. This is genuinely non-linear and cannot be linearized without approximation.

---

## MIN() / MAX() Over Decision Variables

**Priority: Low** (requires Big-M reformulation)

`MIN(x * cost)` and `MAX(x * cost)` over decision variables produce non-linear constraints. They can be linearized using Big-M with auxiliary variables:

For `z = MAX(x_1, x_2, ..., x_n)`:
- Add constraints: `z >= x_i` for all i
- Add constraints: `z <= x_i + M * (1 - y_i)` for all i, where `y_i` is binary
- Add constraint: `SUM(y_i) >= 1`

This requires the Big-M reformulation infrastructure planned in the optimizer (see [../../04_optimizer/query_rewriting/todo.md](../../04_optimizer/query_rewriting/todo.md)).

---

## Division (`/`) Over Decision Variables

**Not planned**. Division by a decision variable is inherently non-linear. Division by a constant is valid but can be handled by multiplying the other side (already possible with current syntax).

---

## NOT Over Decision Variable Expressions

**Not planned**. `NOT` applied to a decision variable expression would require a binary negation auxiliary variable. Use `x = 0` or `1 - x` instead.
