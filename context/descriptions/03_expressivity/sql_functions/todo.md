# SQL Functions & Expressions — Planned Features

---

## COUNT() Over REAL Decision Variables

**Priority: Low**

`COUNT(x)` for REAL variables is not yet supported. The Big-M indicator approach used for INTEGER variables could theoretically work for REAL, but the semantics of "non-zero" for continuous variables are problematic (floating-point tolerance issues). Currently rejected with a clear error message.

---

## ~~AVG() Over Decision Variables~~ — DONE

Implemented. See `done.md` for details. Uses standard SQL AVG semantics (divide by total row count, not "average among selected").

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
