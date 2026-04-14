# Phase 1: Expression Analysis

## Overview

After data materialization (the Sink phase collects all input rows), the first analysis step extracts the structure of constraints and objectives from the bound expression trees. This happens in the `DecideGlobalSinkState` constructor, which calls `AnalyzeConstraint()` and `AnalyzeObjective()` on the bound expressions produced by the binder.

The goal of this phase is purely structural: determine *which* DECIDE variables appear in each constraint/objective and *what* coefficient expressions multiply them. The actual numeric evaluation of those coefficients happens later in Phase 2.

**Key Source File**: `src/execution/operator/decide/physical_decide.cpp` (AnalyzeConstraint/AnalyzeObjective methods in `DecideGlobalSinkState` constructor)
**Header**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`

## `AnalyzeConstraint()`

Recursive traversal of the bound constraint expression tree. Called once per top-level constraint expression, it descends through wrapper layers and ultimately produces `DecideConstraint` structs.

### Wrapper Detection

Constraints may be wrapped in WHEN and/or PER layers (encoded as tagged `BoundConjunctionExpression` nodes):

1. **PER wrapper**: A `BoundConjunctionExpression` with alias `PER_CONSTRAINT_TAG` and 2+ children: `child[0]` is the constraint (possibly further WHEN-wrapped), `child[1..N]` are the PER column expressions. The method extracts the per_columns and recurses into `child[0]`.

2. **WHEN wrapper**: A `BoundConjunctionExpression` with alias `WHEN_CONSTRAINT_TAG` and 2 children: `child[0]` is the actual constraint, `child[1]` is the WHEN condition. The method extracts the when_condition and recurses into `child[0]`.

3. **AND conjunctions**: Regular `BoundConjunctionExpression` nodes (no special alias) represent multiple constraints joined by AND. Each child is recursively analyzed as an independent constraint.

### Comparison Expression Handling

When a `BoundComparisonExpression` is reached (the actual constraint):

- The comparison type (`<=`, `>=`, `=`, etc.) and RHS expression are recorded directly.
- The LHS is unwrapped through any `BoundCastExpression` layers.
- **Aggregate LHS** (e.g., `SUM(x * cost)` or `SUM(x * cost) WHEN a + SUM(x * hours) WHEN b`): `ExtractAggregateConstraintTerms()` walks additive aggregate expressions, calls `ExtractConstraintTerms()` on each aggregate's child, and copies aggregate metadata onto the extracted terms. The `lhs_is_aggregate` flag is set. Aggregate-local `WHEN` filters are stored on `Term::filter`, bilinear term filters, or quadratic group filters. If the aggregate has alias `AVG_REWRITE_TAG`, the extracted terms are marked for AVG scaling.
- **Per-row LHS**: Two sub-paths:
  - **Multi-variable** (RHS also contains DECIDE variables, e.g., `d >= x - c` from ABS linearization): `CollectDecideVarRefs()` finds all DECIDE variables on both sides with their signs. LHS variables keep their sign; RHS variables are moved to LHS with negated sign. `StripDecideVars()` replaces DECIDE variable references in the RHS with constant 0, producing a data-only expression.
  - **Single-variable** (simple bound like `x <= 5`): `FindDecideVariable()` identifies the variable; coefficient is implicitly 1.

## `AnalyzeObjective()`

Extracts terms from the objective's aggregate expression. Handles linear, bilinear, and quadratic objectives, including additive objective expressions with aggregate-local filters.

1. Unwraps any `BoundCastExpression` layers.
2. Checks for a WHEN wrapper (same `WHEN_CONSTRAINT_TAG` pattern) and extracts the condition.
3. Expects a `BoundAggregateExpression` (SUM) or an additive expression containing aggregate terms. The SUM argument is checked for quadratic patterns:
   - `POWER(linear_expr, 2)` / `POW(linear_expr, 2)` / `(expr) ** 2` — exponent is unwrapped from casts (DuckDB wraps the integer literal `2` in a `BoundCastExpression`)
   - `(expr) * (expr)` where both children have identical `ToString()`
   If quadratic: enforces MINIMIZE (throws for MAXIMIZE), sets `has_quadratic = true`, and calls `ExtractTerms()` on the inner linear expression into `squared_terms`.
   If linear: calls `ExtractTerms()` on the SUM argument into `terms`.
4. Stores the result as an `Objective` with terms (linear), squared_terms (quadratic), and optional when_condition/per_columns. Aggregate-local filters are copied onto the terms they came from.

## Variable Bounds Extraction

`ExtractVariableBounds()` and `TraverseBoundsConstraints()` perform a separate pass over the constraint tree to identify simple per-variable bounds (e.g., `x >= 5`, `x <= 10`). These are constraints where:

- The LHS is a bare DECIDE variable (not inside a SUM aggregate)
- The RHS is a constant value

Detected bounds are recorded separately and later applied directly as variable bounds in the solver, which is more efficient than encoding them as general constraints. The traversal:

- Recurses through AND conjunctions, PER wrappers, and WHEN wrappers (examining only `child[0]` for PER/WHEN)
- For comparison expressions: checks that the LHS is not an aggregate, finds the DECIDE variable, extracts the constant RHS value
- Applies the bound: `<=` updates upper_bounds (min), `>=` updates lower_bounds (max), `=` sets both

## Key Data Structures

### `Term` (defined in `physical_decide.hpp`)

```
struct Term {
    idx_t variable_index;              // Which DECIDE variable (INVALID_INDEX for constants)
    unique_ptr<Expression> coefficient; // Row-varying expression to evaluate later
    int sign = 1;                      // +1 or -1 (used by ExtractTerms for subtraction)
    unique_ptr<Expression> filter;      // Optional aggregate-local WHEN filter
    bool avg_scale = false;             // True when term came from AVG
};
```

A single `coeff_expr * x_i` term. The coefficient is an unevaluated expression tree at this stage -- it will be executed against data chunks in Phase 2. The `sign` field tracks negation from subtraction operators.

### `DecideConstraint` (defined in `physical_decide.hpp`)

```
struct DecideConstraint {
    vector<Term> lhs_terms;              // All additive terms from LHS
    unique_ptr<Expression> rhs_expr;     // RHS expression (may contain aggregates)
    ExpressionType comparison_type;       // COMPARE_LESSTHANOREQUALTO or GREATERTHANOREQUALTO
    bool lhs_is_aggregate = false;        // True if original LHS was an aggregate (e.g., SUM(...))
    idx_t minmax_indicator_idx = DConstants::INVALID_INDEX;  // Indicator var idx for hard MIN/MAX
    string minmax_agg_type;              // "min" or "max" (empty if not minmax)
    idx_t ne_indicator_idx = DConstants::INVALID_INDEX;      // Indicator var idx for not-equal (<>)
    unique_ptr<Expression> when_condition; // Optional WHEN condition (nullptr = unconditional)
    vector<unique_ptr<Expression>> per_columns; // Optional PER grouping columns (empty = no grouping)
    vector<BilinearConstraintTerm> bilinear_terms; // Bilinear aggregate terms
    vector<QuadraticGroup> quadratic_groups; // Quadratic aggregate groups
};
```

### `Objective` (defined in `physical_decide.hpp`)

```
struct Objective {
    vector<Term> terms;                    // Linear objective terms
    unique_ptr<Expression> when_condition; // Optional WHEN condition (nullptr = unconditional)
    vector<unique_ptr<Expression>> per_columns; // Optional PER grouping columns (empty = no grouping)
    vector<Term> squared_terms;            // Inner linear terms for QP: SUM(POWER(expr, 2))
    bool has_quadratic = false;            // True if objective is quadratic
    vector<BilinearTerm> bilinear_terms;   // Bilinear objective terms
};
```

## Helper Functions

All methods on `PhysicalDecide`:

- **`FindDecideVariable(expr)`**: Recursively searches the expression tree for a `BoundColumnRefExpression` whose binding matches any DECIDE variable. Returns the variable index or `INVALID_INDEX`.

- **`ContainsVariable(expr, var_idx)`**: Checks whether the expression tree contains a reference to a specific DECIDE variable. Used by `ExtractCoefficientWithoutVariable`.

- **`ExtractCoefficientWithoutVariable(expr, var_idx)`**: Given a multiplication expression containing a DECIDE variable, returns a copy with the variable factor removed. For example, from `x * 5 * l_tax`, removes `x` and returns `5 * l_tax`. If the expression *is* the variable itself, returns constant 1.

- **`ExtractTerms(expr, out_terms)`**: Main visitor for decomposing SUM arguments into terms. Handles:
  - `+` operators: recursively processes all children
  - `-` operators (binary): processes first child, then second child with sign flipped
  - `*` operators: finds the DECIDE variable (if any) and extracts the coefficient
  - Cast expressions: recurses into the child
  - Base case (column ref or constant): either a bare variable (coefficient = 1) or a constant term

- **`ExtractAggregateConstraintTerms(expr, constraint, sign)`** and **`ExtractAggregateObjectiveTerms(expr, objective, sign)`**: Walk additive expressions of aggregate terms. Each `BoundAggregateExpression` must already be rewritten to SUM by the optimizer. These helpers copy aggregate-local `BoundAggregateExpression::filter` into the extracted term metadata and mark terms that came from `AVG_REWRITE_TAG` for Phase 2 scaling.

Static helper functions (not on `PhysicalDecide`):

- **`CollectDecideVarRefs(expr, sign, refs, op)`**: Walks the expression tree tracking sign through `+` and `-` operators, collecting all DECIDE variable references with their accumulated sign (+1 or -1).

- **`StripDecideVars(expr, op)`**: Returns a copy of the expression with all DECIDE variable references replaced by constant `0.0`. Produces a data-only expression suitable for evaluation via `ExpressionExecutor`.
