# Query Rewriting — Implemented Features

---

## WHERE-Clause Filtering

Standard DuckDB predicate pushdown applies before the DECIDE clause. Rows eliminated by WHERE never enter the constraint/objective matrix. This is not PackDB-specific — it's inherited from DuckDB's optimizer.

---

## WHEN-Condition Coefficient Zeroing

When a constraint or objective has a `WHEN` modifier, non-matching rows have their coefficients set to zero in the solver matrix. This is done during physical execution, not as a query rewrite.

**How it works**:
1. The WHEN condition is evaluated per-row to produce a boolean mask
2. For aggregate constraints: coefficients are multiplied by the mask (0 for non-matching rows)
3. For per-row constraints: the constraint row is omitted entirely for non-matching rows
4. For objectives: the objective coefficient is zeroed for non-matching rows

**Code**: `src/execution/operator/decide/physical_decide.cpp:200-203, 263-282`

---

## What's Not Done

The current system performs **no COP-specific query rewriting**. Constraints and objectives are passed to the solver exactly as the user wrote them, with only WHEN masking applied. See [todo.md](todo.md) for planned rewrites.
