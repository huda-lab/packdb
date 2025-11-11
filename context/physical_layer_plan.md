# Physical Layer To-Do (DECIDE)

The binder now produces richer bound expressions for DECIDE constraints and objectives. The physical layer must be updated to consume these safely.

---

## Current Inputs from the Binder

- **Constraints** arrive as:
  - LHS: a single `BoundAggregateExpression` for `SUM(...)`, whose child tree expands into additive terms like `x * (5 * l_tax + 5 * l_discount)` or `y * (-3 * l_extendedprice)`.
  - RHS: scalar constant plus zero or more `SUM(-row_term)` aggregates (each one a `BoundAggregateExpression` with a negated inner expression).
- **Objectives** arrive as a single `BoundAggregateExpression` that contains additive, linear terms in the DECIDE variables (e.g., `x * (6 * l_extendedprice) + y * (4 * l_discount)`).
- Debug helpers (`PACKDB_DEBUG_BINDER`) can print `Expression::ToString()` for both sides to aid inspection.

These trees are linear but no longer factored into “coefficient × column” pairs. Execution must interpret them structurally rather than relying on the old `AnalyzeSumArgument`.

---

## Required Physical Updates

1. **New analyzers**
   - Replace `PhysicalDecide::AnalyzeConstraint` / `AnalyzeObjective` logic with a visitor that can:
     * Walk a `BoundAggregateExpression` and extract linear terms of the form `variable * row_expression`.
     * Collect row-only aggregates on the RHS and move their contribution to the constant term.
   - Guard against unsupported nodes (non-linear combinations, additional aggregates).

2. **Canonical representation**
   - Decide on an internal struct (e.g., vector of `DecideTerm { idx_t variable_idx; unique_ptr<Expression> row_expr; double coeff; }`) to store each constraint/objective term once extracted.
   - Update the logical/physical operators to store this canonical form so execution and future solver code can reuse it.

3. **Error reporting**
   - If the physical analyzer encounters a pattern the binder promised to filter out, emit a descriptive runtime error rather than dereferencing null pointers.

4. **Testing & Debugging**
   - Add targeted tests once the physical layer is updated: run the sample DECIDE queries and confirm they reach execution without crashing.
   - Keep binder debug prints enabled while developing the physical side; remove or gate them after the analyzer stabilizes.

---

## Out of Scope (for now)

- Solver integration (actual optimisation/decision logic).
- Support for equality or BETWEEN constraints.
- Non-linear expressions or subqueries inside DECIDE clauses.

This document should be revisited once the physical analyzer is in place and we are ready to wire in a solver or further features.
