# Phase 3: Model Building

## Overview

The model builder transforms the solver-agnostic `SolverInput` (evaluated constraints with numeric coefficients) into a `SolverModel` (flat variable arrays + constraint list in COO format + optional Q matrix for QP). This is the bridge between PackDB's domain model and the generic optimization formulation that any solver backend can consume.

The core logic lives in `SolverModel::Build()`, a static method that takes a `SolverInput` and returns a fully constructed `SolverModel`.

**Key Source File**: `src/packdb/utility/ilp_model_builder.cpp` (~384 lines)
**Headers**: `src/include/duckdb/packdb/ilp_model.hpp`, `src/include/duckdb/packdb/solver_input.hpp`

## Variable Setup

The ILP has `num_rows * num_decide_vars` solver variables. Each DECIDE variable is replicated once per row, forming a 2D grid flattened into a 1D array indexed as `row * num_decide_vars + var_idx`.

### Per-Variable Type and Default Bounds

Each DECIDE variable's logical type determines its solver properties:

| Logical Type | `is_binary` | `is_integer` | Default Bounds |
|---|---|---|---|
| BOOLEAN | true | true | [0, 1] |
| INTEGER / BIGINT | false | true | [0, 1e30] |
| DOUBLE / FLOAT (REAL) | false | false | [0, 1e30] |

### Explicit Bounds Intersection

Explicit bounds from `SolverInput` (extracted from simple constraints like `x >= 5`, `x <= 10` during Phase 1) are intersected with the type-based defaults:

- Lower bound: `max(type_default_lower, explicit_lower)`
- Upper bound: `min(type_default_upper, explicit_upper)`

### Expansion to Solver Variables

The per-variable configuration is expanded to all `num_rows * num_decide_vars` solver variables. Every row gets the same bounds and type for a given DECIDE variable.

## Objective Coefficient Array

The objective is represented as a flat `obj_coeffs` vector of size `total_vars`, initialized to 0.0. For each objective term:

- `objective_variable_indices[term_idx]` identifies the DECIDE variable.
- `objective_coefficients[term_idx][row]` provides the per-row coefficient.
- Each coefficient is placed at index `row * num_decide_vars + decide_var_idx`.

The `maximize` flag is set from `input.sense`.

## Constraint Building

Each `EvaluatedConstraint` is converted to one or more `ModelConstraint` structs. The path depends on whether the constraint is aggregate (SUM-level) and whether it has groups (WHEN/PER).

### Path 1: Aggregate, Ungrouped (No WHEN, No PER)

Fast path when `row_group_ids` is empty. A single `ModelConstraint` is produced that sums over all rows:

- For each term with a valid variable index, all rows contribute: variable index `row * num_decide_vars + decide_var_idx`, coefficient from `row_coefficients[term_idx][row]`.
- RHS comes from `rhs_values[0]`.
- If `was_avg_rewrite` is true, the RHS is scaled by `num_rows` (converting AVG semantics to SUM).

### Path 2: Aggregate, Grouped (WHEN and/or PER)

A `group_to_rows` index is built: for each group ID, collect which rows belong to it (skipping `INVALID_INDEX` rows). Then one `ModelConstraint` is emitted per non-empty group:

- Only rows in the group contribute coefficients.
- RHS comes from `rhs_values[0]`.
- If `was_avg_rewrite` is true, the RHS is scaled by the group's row count (`group_rows[g].size()`), not the total row count. This correctly computes AVG per group.

### Path 3: Per-Row

One `ModelConstraint` per row. Rows with `row_group_ids[row] == INVALID_INDEX` (excluded by WHEN) are skipped. Each constraint contains only the terms for that single row, with RHS from `rhs_values[row]`.

## `ApplyComparisonSense()`

Converts DuckDB's `ExpressionType` comparisons to ILP constraint sense characters:

| ExpressionType | Sense Char | RHS Treatment |
|---|---|---|
| `COMPARE_GREATERTHANOREQUALTO` | `'>'` (meaning >=) | Direct |
| `COMPARE_GREATERTHAN` | `'>'` | `floor(rhs) + 1.0` (strict to non-strict for integers) |
| `COMPARE_LESSTHANOREQUALTO` | `'<'` (meaning <=) | Direct |
| `COMPARE_LESSTHAN` | `'<'` | `ceil(rhs) - 1.0` (strict to non-strict for integers) |
| `COMPARE_EQUAL` | `'='` | Direct |

Note: The sense characters `'>'` and `'<'` represent `>=` and `<=` respectively (standard ILP convention). Strict inequalities are converted by adjusting the RHS, which is exact for integer variables but an approximation for continuous variables.

## Quadratic Objective (Q Matrix)

When `input.has_quadratic_objective` is true, the model builder constructs the Q matrix for the standard QP form `minimize (1/2) x^T Q x + c^T x`.

The inner linear expression of `SUM(POWER(expr, 2))` has already been evaluated per-row in `SolverInput::quadratic_inner_coefficients`. For each row, the inner expression is `sum_t(a_{t,row} * x_{var_t})`. The Q matrix is built by summing the outer products across all rows:

1. **Variable terms**: For each row, collect non-zero coefficients for each DECIDE variable. Build Q entries from the outer product: `Q[i,j] += 2 * a_i * a_j` (factor of 2 on all entries — both diagonal and off-diagonal — due to the `(1/2) x^T Q x` convention).

2. **Constant terms**: If the inner expression has a constant part `c`, the expansion `(expr + c)^2 = expr^2 + 2c·expr + c^2` produces linear cross-terms `2c·a_t` that are added to `obj_coeffs`.

3. **Storage**: Q is accumulated in a `std::map<pair<int,int>, double>` (lower triangle, row >= col) then serialized to COO vectors (`q_rows`, `q_cols`, `q_vals`).

**Convexity guarantee**: Because Q = sum(a·a^T) = A^T A, it is always positive semidefinite by construction. This is the key payoff of the syntax-enforced convexity design.

## `ModelConstraint` Format

Each constraint is stored in COO (coordinate) format:

```
struct ModelConstraint {
    vector<int> indices;        // Variable indices into the flattened array
    vector<double> coefficients; // Coefficient for each variable
    char sense;                 // '<' (<=), '>' (>=), '=' (==)
    double rhs;                 // Right-hand side value
};
```

This format is consumed directly by Gurobi (`GRBaddconstr` takes COO) and converted to CSR for HiGHS.

## `SolverModel` Structure

```
struct SolverModel {
    idx_t num_vars;              // total_vars = num_rows * num_decide_vars
    vector<double> col_lower;    // Lower bounds per solver variable
    vector<double> col_upper;    // Upper bounds per solver variable
    vector<bool> is_integer;     // True for INTEGER/BOOLEAN, false for REAL
    vector<bool> is_binary;      // True for BOOLEAN (subset of integer)
    vector<double> obj_coeffs;   // Linear objective coefficient per variable
    bool maximize;               // True = maximize, false = minimize
    vector<int> q_rows;          // Q matrix row indices (COO, lower triangle)
    vector<int> q_cols;          // Q matrix column indices (COO, lower triangle)
    vector<double> q_vals;       // Q matrix values
    bool has_quadratic_obj;      // True if QP objective present
    vector<ModelConstraint> constraints;
};
```

## Sanity Checks

After building, the model is validated:

1. **Bounds**: Every variable must have finite lower and upper bounds, with `lower <= upper`. Violation throws `InternalException` with the column index and bound values.

2. **Objective coefficients**: Every objective coefficient must be finite (not NaN or Infinity).

3. **Constraint validity**: For each constraint:
   - `indices` and `coefficients` vectors must be the same size.
   - Every variable index must be within `[0, total_vars)`.
   - Every coefficient must be finite.
   - RHS must not be NaN (infinity is allowed for range-based representations).

These checks catch bugs in the evaluation or model-building logic before they propagate to the solver, where error messages would be less informative.
