# Binder Layer Documentation

## Overview
The Binder Layer is responsible for validating the semantically normalized DECIDE clauses and converting them into bound expression trees that can be used by the Logical Planner. It ensures that constraints and objectives adhere to the rules of the package query system (e.g., linearity, variable scope).

**Source Files**:
- `src/planner/expression_binder/decide_binder.cpp`
- `src/planner/expression_binder/decide_constraints_binder.cpp`
- `src/planner/expression_binder/decide_objective_binder.cpp`

## Architecture

### `DecideBinder` (Base Class)
Extends DuckDB's `ExpressionBinder`. It manages the scope of DECIDE variables and provides shared validation logic.
- **`ValidateSumArgument`**: A critical helper that enforces linearity. It ensures that:
    - Arguments are linear combinations of DECIDE variables.
    - No nested `SUM` functions are allowed.
    - No non-linear terms (e.g., `x * x` or `x * y`) are present.
    - At least one DECIDE variable is present in the expression.

### `DecideConstraintsBinder`
Binds the `SUCH THAT` clause.
- **Constraint Shapes**:
    - **Variable Constraints**: `x <= c`, `x >= c`, `x IN (...)`.
    - **Sum Constraints**: `SUM(linear_expr) <= rhs`, `SUM(linear_expr) >= rhs`.
- **RHS Validation**: The Right-Hand Side (RHS) must be a scalar or an aggregate expression that does **not** contain any DECIDE variables.
- **Type Refinement**: It detects type declarations like `x IS INTEGER` and updates the variable's type in the binding context.
- **Limitations**: Currently, equality (`=`) and `BETWEEN` constraints are explicitly rejected with a "not supported" error, though the underlying logic could support them.

### `DecideObjectiveBinder`
Binds the `[MAXIMIZE|MINIMIZE]` clause.
- **Validation**: Ensures the objective is a single `SUM(...)` expression containing at least one DECIDE variable.
- **Sense**: Records whether the goal is to MAXIMIZE or MINIMIZE.

## Data Flow
1. **Input**: `ParsedExpression` trees from the Parser (already normalized).
2. **Processing**:
    - Variables are registered in the binding context.
    - Constraints are bound and validated.
    - Variable types are refined based on constraints.
    - Objective is bound.
3. **Output**: `BoundSelectNode` containing:
    - `decide_variables`: Vector of `BoundColumnRefExpression`.
    - `decide_constraints`: Bound expression tree.
    - `decide_objective`: Bound expression tree.

## Integration
The output of the Binder is passed to the Logical Planner, which creates a `LogicalDecide` operator. This operator sits above the standard query plan, carrying the bound constraints and objective to the execution layer.
