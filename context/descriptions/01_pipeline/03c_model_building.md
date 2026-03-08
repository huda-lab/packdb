# Phase 3: Model Building

## Overview

The model builder transforms the solver-agnostic `SolverInput` (evaluated constraints with numeric coefficients) into an `ILPModel` (flat variable arrays + constraint list in COO format). This is the bridge between PackDB's domain model and the generic ILP formulation that any solver backend can consume.

The core logic lives in `ILPModel::Build()`, a static method that takes a `SolverInput` and returns a fully constructed `ILPModel`.

**Key Source File**: `src/packdb/utility/ilp_model_builder.cpp` (~247 lines)
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

Each `EvaluatedConstraint` is converted to one or more `ILPConstraint` structs. The path depends on whether the constraint is aggregate (SUM-level) and whether it has groups (WHEN/PER).

### Path 1: Aggregate, Ungrouped (No WHEN, No PER)

Fast path when `row_group_ids` is empty. A single `ILPConstraint` is produced that sums over all rows:

- For each term with a valid variable index, all rows contribute: variable index `row * num_decide_vars + decide_var_idx`, coefficient from `row_coefficients[term_idx][row]`.
- RHS comes from `rhs_values[0]`.
- If `was_avg_rewrite` is true, the RHS is scaled by `num_rows` (converting AVG semantics to SUM).

### Path 2: Aggregate, Grouped (WHEN and/or PER)

A `group_to_rows` index is built: for each group ID, collect which rows belong to it (skipping `INVALID_INDEX` rows). Then one `ILPConstraint` is emitted per non-empty group:

- Only rows in the group contribute coefficients.
- RHS comes from `rhs_values[0]`.
- If `was_avg_rewrite` is true, the RHS is scaled by the group's row count (`group_rows[g].size()`), not the total row count. This correctly computes AVG per group.

### Path 3: Per-Row

One `ILPConstraint` per row. Rows with `row_group_ids[row] == INVALID_INDEX` (excluded by WHEN) are skipped. Each constraint contains only the terms for that single row, with RHS from `rhs_values[row]`.

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

## `ILPConstraint` Format

Each constraint is stored in COO (coordinate) format:

```
struct ILPConstraint {
    vector<int> indices;        // Variable indices into the flattened array
    vector<double> coefficients; // Coefficient for each variable
    char sense;                 // '<' (<=), '>' (>=), '=' (==)
    double rhs;                 // Right-hand side value
};
```

This format is consumed directly by Gurobi (`GRBaddconstr` takes COO) and converted to CSR for HiGHS.

## `ILPModel` Structure

```
struct ILPModel {
    idx_t num_vars;              // total_vars = num_rows * num_decide_vars
    vector<double> col_lower;    // Lower bounds per solver variable
    vector<double> col_upper;    // Upper bounds per solver variable
    vector<bool> is_integer;     // True for INTEGER/BOOLEAN, false for REAL
    vector<bool> is_binary;      // True for BOOLEAN (subset of integer)
    vector<double> obj_coeffs;   // Objective coefficient per solver variable
    bool maximize;               // True = maximize, false = minimize
    vector<ILPConstraint> constraints;
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
