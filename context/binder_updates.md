# Binder Normalization & Binding Updates (WIP)

## Overview

Recent work extended the symbolic pre-processor and the binder to accept richer DECIDE constraint/objective shapes. The parser now:

- Converts entire constraint LHS expressions to symbolic form.
- Keeps all DECIDE-variable terms inside a single `SUM(...)` on the left-hand side.
- Moves any purely row-varying aggregates/constants to the right-hand side with negated arguments, while preserving original numeric coefficients.

The binder had to be relaxed to recognize and allow these new patterns.

## Constraint Binder (`DecideConstraintsBinder`)

- **RHS acceptance**: `IsAllowedConstraintRHS` permits arithmetic trees containing constants, casts, and `SUM` aggregates so long as no DECIDE variables appear (enforced with the shared `ExpressionContainsDecideVariable` helper). Subtraction inside RHS expressions is rejected; explicit `-1 * term` patterns are preferred.
- **SUM validation**: `ValidateSumArgument` walks the entire SUM argument tree, allowing nested `+` and `*` plus casts. It ensures at least one DECIDE variable is present, forbids nested `SUM` calls, and raises on any multiplicative sub-expression containing more than one DECIDE variable (`x*x`, `x*y`, etc.).
- **Comparison handling (Phase 2)**:  
  - Only `<=` and `>=` comparisons are accepted; equality and BETWEEN clauses now produce a clear “not supported” binder error.  
  - The binder verifies the LHS is exactly `SUM(...)`.  
  - RHS expressions are simplified in-place to drop `0 + expr` patterns before binding so the downstream plan receives cleaner trees.

### Still Missing

- Equalities/BETWEEN clauses have not been revisited beyond sharing the relaxed RHS checks; more complex forms may still be rejected until we add targeted tests.
- The binder still enforces `SUM` on the LHS; expressions that omit the aggregate wrapper will be rejected (by design).

## Objective Binder (`DecideObjectiveBinder`)

- Shares the new `ValidateSumArgument` path, allowing objectives such as `SUM(6*x*l_extendedprice + 4*y*l_discount)` while rejecting nonlinear products.
- Continues to require a single `SUM(...)` whose argument references at least one DECIDE variable.

## Outstanding Work

- **Canonical hand-off (TODO)**: the binder still delivers raw bound expressions. Phase 3 will introduce a canonical structure (e.g., explicit coefficient/row-term pairs) plus TODO markers so the physical layer can consume constraints/objectives without tree spelunking.
- **Physical layer**: `PhysicalDecide::AnalyzeConstraint` and `AnalyzeSumArgument` assume the older `SUM(f(row) * variable)` shape and currently crash when given the new bound trees. They must be updated to:
  - Traverse `BoundAggregateExpression` nodes whose children are now additive/multiplicative combinations of DECIDE variables and row expressions.
  - Support multiple DECIDE variables per aggregate.
  - Handle RHS aggregates produced by normalization.
- **Negative constants / nested casts**: Additional tests are needed to ensure every combination of casts and unary minus passes through binding and into execution.
- **Error messaging**: Binder error strings still reference the old limitations (e.g., “Either SUM(x), SUM(f(a)*x) or SUM(x*f(a)) is allowed”). These should be refreshed once the execution layer is updated.

## Test Coverage

The normalized queries in `test/packdb/test.sql` now pass binding, but running them still fails inside the DECIDE physical operator because the analyzer cannot interpret the richer bound aggregates yet. Documentation of this limitation is kept to highlight the next development step.
