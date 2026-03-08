# Phase 1: Expression Analysis

## Overview

After data materialization (the Sink phase collects all input rows), the first analysis step extracts the structure of constraints and objectives from the bound expression trees. This happens in the `DecideGlobalSinkState` constructor, which calls `AnalyzeConstraint()` and `AnalyzeObjective()` on the bound expressions produced by the binder.

The goal of this phase is purely structural: determine *which* DECIDE variables appear in each constraint/objective and *what* coefficient expressions multiply them. The actual numeric evaluation of those coefficients happens later in Phase 2.

**Key Source File**: `src/execution/operator/decide/physical_decide.cpp` (lines ~260-506)
**Header**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`

## `AnalyzeConstraint()`

Recursive traversal of the bound constraint expression tree. Called once per top-level constraint expression, it descends through wrapper layers and ultimately produces `LinearConstraint` structs.

### Wrapper Detection

Constraints may be wrapped in WHEN and/or PER layers (encoded as tagged `BoundConjunctionExpression` nodes):

1. **PER wrapper**: A `BoundConjunctionExpression` with alias `PER_CONSTRAINT_TAG` and 2 children: `child[0]` is the constraint (possibly further WHEN-wrapped), `child[1]` is the PER column expression. The method extracts the per_column and recurses into `child[0]`.

2. **WHEN wrapper**: A `BoundConjunctionExpression` with alias `WHEN_CONSTRAINT_TAG` and 2 children: `child[0]` is the actual constraint, `child[1]` is the WHEN condition. The method extracts the when_condition and recurses into `child[0]`.

3. **AND conjunctions**: Regular `BoundConjunctionExpression` nodes (no special alias) represent multiple constraints joined by AND. Each child is recursively analyzed as an independent constraint.

### Comparison Expression Handling

When a `BoundComparisonExpression` is reached (the actual constraint):

- The comparison type (`<=`, `>=`, `=`, etc.) and RHS expression are recorded directly.
- The LHS is unwrapped through any `BoundCastExpression` layers.
- **Aggregate LHS** (e.g., `SUM(x * cost)`): `ExtractLinearTerms()` is called on the aggregate's first child to decompose it into `LinearTerm` structs. The `lhs_is_aggregate` flag is set. If the aggregate has alias `AVG_REWRITE_TAG`, the `was_avg_rewrite` flag is also set (RHS will need scaling later).
- **Per-row LHS**: Two sub-paths:
  - **Multi-variable** (RHS also contains DECIDE variables, e.g., `d >= x - c` from ABS linearization): `CollectDecideVarRefs()` finds all DECIDE variables on both sides with their signs. LHS variables keep their sign; RHS variables are moved to LHS with negated sign. `StripDecideVars()` replaces DECIDE variable references in the RHS with constant 0, producing a data-only expression.
  - **Single-variable** (simple bound like `x <= 5`): `FindDecideVariable()` identifies the variable; coefficient is implicitly 1.

## `AnalyzeObjective()`

Similar to constraint analysis but simpler -- extracts terms from the objective's SUM argument.

1. Unwraps any `BoundCastExpression` layers.
2. Checks for a WHEN wrapper (same `WHEN_CONSTRAINT_TAG` pattern) and extracts the condition.
3. Expects a `BoundAggregateExpression` (SUM). Calls `ExtractLinearTerms()` on the aggregate's first child.
4. Stores the result as a `LinearObjective` with terms and optional when_condition.

## Variable Bounds Extraction

`ExtractVariableBounds()` and `TraverseBoundsConstraints()` perform a separate pass over the constraint tree to identify simple per-variable bounds (e.g., `x >= 5`, `x <= 10`). These are constraints where:

- The LHS is a bare DECIDE variable (not inside a SUM aggregate)
- The RHS is a constant value

Detected bounds are recorded separately and later applied directly as variable bounds in the solver, which is more efficient than encoding them as general constraints. The traversal:

- Recurses through AND conjunctions, PER wrappers, and WHEN wrappers (examining only `child[0]` for PER/WHEN)
- For comparison expressions: checks that the LHS is not an aggregate, finds the DECIDE variable, extracts the constant RHS value
- Applies the bound: `<=` updates upper_bounds (min), `>=` updates lower_bounds (max), `=` sets both

## Key Data Structures

### `LinearTerm` (defined in `physical_decide.hpp`)

```
struct LinearTerm {
    idx_t variable_index;              // Which DECIDE variable (INVALID_INDEX for constants)
    unique_ptr<Expression> coefficient; // Row-varying expression to evaluate later
};
```

A single `coeff_expr * x_i` term. The coefficient is an unevaluated expression tree at this stage -- it will be executed against data chunks in Phase 2.

### `LinearConstraint` (defined in `physical_decide.hpp`)

```
struct LinearConstraint {
    vector<LinearTerm> lhs_terms;        // All additive terms from LHS
    unique_ptr<Expression> rhs_expr;     // RHS expression (may be constant or row-varying)
    ExpressionType comparison_type;       // <=, >=, =, etc.
    bool lhs_is_aggregate = false;        // True if original LHS was SUM(...)
    bool was_avg_rewrite = false;         // True if originally AVG (RHS needs scaling)
    unique_ptr<Expression> when_condition; // Optional WHEN condition (nullptr = unconditional)
    unique_ptr<Expression> per_column;     // Optional PER grouping column (nullptr = no grouping)
};
```

### `LinearObjective` (defined in `physical_decide.hpp`)

```
struct LinearObjective {
    vector<LinearTerm> terms;              // All objective terms
    unique_ptr<Expression> when_condition; // Optional WHEN condition (nullptr = unconditional)
};
```

## Helper Functions

All methods on `PhysicalDecide`:

- **`FindDecideVariable(expr)`**: Recursively searches the expression tree for a `BoundColumnRefExpression` whose binding matches any DECIDE variable. Returns the variable index or `INVALID_INDEX`.

- **`ContainsVariable(expr, var_idx)`**: Checks whether the expression tree contains a reference to a specific DECIDE variable. Used by `ExtractCoefficientWithoutVariable`.

- **`ExtractCoefficientWithoutVariable(expr, var_idx)`**: Given a multiplication expression containing a DECIDE variable, returns a copy with the variable factor removed. For example, from `x * 5 * l_tax`, removes `x` and returns `5 * l_tax`. If the expression *is* the variable itself, returns constant 1.

- **`ExtractLinearTerms(expr, out_terms)`**: Main visitor for decomposing SUM arguments into linear terms. Handles:
  - `+` operators: recursively processes all children
  - `*` operators: finds the DECIDE variable (if any) and extracts the coefficient
  - Cast expressions: recurses into the child
  - Base case (column ref or constant): either a bare variable (coefficient = 1) or a constant term

Static helper functions (not on `PhysicalDecide`):

- **`CollectDecideVarRefs(expr, sign, refs, op)`**: Walks the expression tree tracking sign through `+` and `-` operators, collecting all DECIDE variable references with their accumulated sign (+1 or -1).

- **`StripDecideVars(expr, op)`**: Returns a copy of the expression with all DECIDE variable references replaced by constant `0.0`. Produces a data-only expression suitable for evaluation via `ExpressionExecutor`.
