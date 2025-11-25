# Physical Layer Documentation

## Overview
The Physical Layer executes the package query by integrating with the HiGHS solver. It transforms the bound expression trees into a linear programming (ILP) model, solves it, and produces the final result set.

**Source File**: `src/execution/operator/decide/physical_decide.cpp`

## Execution Pipeline

### Phase 1: Expression Analysis
The `PhysicalDecide` operator first analyzes the bound constraints and objective to understand their structure.
- **Visitor Pattern**: It uses a visitor pattern to traverse expression trees.
- **`LinearTerm`**: Identifies terms as pairs of `{variable_index, coefficient_expression}`.
- **`ExtractLinearTerms`**: Recursively extracts these terms from additive expressions.
- **`AnalyzeConstraint` / `AnalyzeObjective`**: Converts complex bound expressions into a structured `LinearConstraint` or `LinearObjective` format, separating the decision variables from the row-varying coefficients.
- **Implicit Casts**: Handles implicit casts (e.g., `CAST(SUM(...) AS DOUBLE)`) on the LHS of constraints to correctly identify aggregate constraints.

### Phase 2: Coefficient Evaluation
Before solving, the operator must evaluate the row-varying coefficients (e.g., `price * tax`) for every row in the input dataset.
- **`EvaluatedConstraint`**: Stores the evaluated numeric coefficients for each term across all rows.
- **`ExpressionExecutor`**: Used to evaluate the coefficient expressions against the input `DataChunk`s.
- **Result**: A matrix of coefficients (`[term_idx][row_idx]`) and a vector of RHS values.

### Phase 3: Solver Integration (HiGHS)
The operator builds and solves the ILP model using the HiGHS library.
- **Variables**: One solver variable is created for each combination of (row, decision_variable).
- **Variable Types**: Variables are set to `INTEGER` or `BINARY` based on their type definition.
- **Constraints**: Linear inequalities are added to the model based on the evaluated coefficients.
    - **Aggregate Constraints**: `SUM(...)` constraints sum over all rows.
    - **Row-wise Constraints**: (If implemented) would constrain individual rows.
- **Objective**: The objective function coefficients are set for each variable.

### Phase 4: Solution Extraction
Once the solver returns an optimal solution:
- **`GetData`**: The operator scans the input data again (buffered in `DecideGlobalSourceState`).
- **Mapping**: It maps the flat solution vector from HiGHS back to the corresponding rows and decision variables.
- **Output**: It appends the decision variable values to the output `DataChunk`s, effectively "deciding" the package.

## Key Data Structures
- **`DecideGlobalSinkState`**: Buffers all input data and holds the analysis/evaluation results.
- **`LinearConstraint`**: Structural representation of a constraint (before evaluation).
- **`EvaluatedConstraint`**: Numeric representation of a constraint (after evaluation).
