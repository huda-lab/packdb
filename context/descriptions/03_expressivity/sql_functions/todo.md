# SQL Functions & Expressions — Planned Features

---

## COUNT() Over REAL Decision Variables

**Priority: Low**

`COUNT(x)` for REAL variables is not yet supported. The Big-M indicator approach used for INTEGER variables could theoretically work for REAL, but the semantics of "non-zero" for continuous variables are problematic (floating-point tolerance issues). Currently rejected with a clear error message.

---

## Division (`/`) Over Decision Variables

**Not planned**. Division by a decision variable is inherently non-linear. Division by a constant is valid but can be handled by multiplying the other side (already possible with current syntax).

---

## NOT Over Decision Variable Expressions

**Not planned**. `NOT` applied to a decision variable expression would require a binary negation auxiliary variable. Use `x = 0` or `1 - x` instead.

---

## IN on Aggregates

**Not planned**. `SUM(x) IN (...)` is not supported. Use multiple equality constraints or BETWEEN instead.
