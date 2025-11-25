# Parser & Symbolic Layer Documentation

## Overview
The Parser and Symbolic layer is responsible for the initial processing and normalization of DECIDE clauses. It acts as a pre-processing step before the Binder, ensuring that complex algebraic expressions are rewritten into a canonical form that separates decision variables from row-varying terms.

**Source File**: `src/packdb/symbolic/decide_symbolic.cpp`

## Key Concepts

### Symbolic Translation
The core mechanism involves converting DuckDB's `ParsedExpression` tree into a symbolic representation (using `SymbolicC++`) and back. This allows for algebraic simplification and rearrangement.

- **`ToSymbolicRecursive`**: Converts a `ParsedExpression` into a `Symbolic` object. It handles arithmetic operators, constants, and column references.
- **`FromSymbolic`**: Converts a `Symbolic` object back into a `ParsedExpression`.
- **`SymbolicTranslationContext`**: Carries the set of DECIDE variable names, allowing the translator to distinguish between decision variables (which become symbolic variables) and table columns (which are treated as constants or row-varying terms).

### Normalization Logic

The goal of normalization is to produce a predictable structure for the Binder.

#### 1. Constraint Normalization
For constraints (e.g., `SUM(x * price) <= 100`), the normalizer:
1. Converts the LHS to symbolic form.
2. Flattens the expression into additive terms.
3. Separates terms into:
    - **Decision Terms**: Contain at least one DECIDE variable.
    - **Row Terms**: Contain NO DECIDE variables.
    - **Constants**.
4. Rebuilds the constraint as:
   ```
   SUM(decision_terms) [<=|>=] rhs_constant + SUM(-row_terms)
   ```
   - All decision variables stay on the LHS inside a single `SUM`.
   - All row-varying terms are moved to the RHS (negated).
   - Constants are accumulated on the RHS.

**Note**: Coefficients are preserved exactly as is; no GCD extraction or scaling is performed.

#### 2. Objective Normalization
Objectives (e.g., `MAXIMIZE SUM(x * profit)`) are normalized similarly:
1. The inner expression of the `SUM` is converted to symbolic form.
2. Terms containing DECIDE variables are kept.
3. Terms without DECIDE variables are dropped (as they don't affect the optimization decision).
4. The `SUM` is rebuilt with only the relevant terms.

## Interaction with Binder
The Binder relies on this normalization. It expects:
- **Constraints**: LHS is a single `SUM(...)` containing only DECIDE variables. RHS is a scalar or aggregate expression without DECIDE variables.
- **Objectives**: A single `SUM(...)` containing at least one DECIDE variable.

## Limitations
- **Non-Linearity**: The symbolic layer can represent non-linear terms (e.g., `x*x`), but the Binder will later reject them if they violate the linearity requirement of the solver.
- **Functions**: Only basic arithmetic and `SUM` are fully supported for symbolic manipulation. Other functions are treated as opaque symbols or rejected.
