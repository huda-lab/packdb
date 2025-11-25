# Pipeline Issues & Critical Gaps

## Critical Blockers (Must Fix)

### 1. Variable Bounds and Types Extraction (Phase 0 Gap)
**Status**: **FIXED**
- **Fix**: Verified that `PhysicalDecide` correctly extracts types and bounds. Added verification tests.

## Missing Features

### 2. Equality Constraints
**Status**: **FIXED**
- **Fix**: Updated `DecideConstraintsBinder` to allow `COMPARE_EQUAL`.

### 3. BETWEEN Constraints
**Status**: **FIXED**
- **Fix**: Updated `DecideConstraintsBinder` to transform `BETWEEN` into `>=` AND `<=`.

### 4. Subquery Support
- **Issue**: Subqueries in the `DECIDE` clause (e.g., `sum(x * (SELECT ...))`) were not supported.
- **Status**: **FIXED**
- **Fix**: Implemented bind-time execution of uncorrelated scalar subqueries in `DecideBinder`. Updated `PhysicalDecide` to handle implicit casts and `decide_symbolic.cpp` to preserve subqueries during normalization.
- **Verification**: `test/sql/decide_subquery.test` passes.
## Solver Integration Status
- **HiGHS Integration**: Working. The solver is correctly linked and called.
- **Model Building**: Working for basic linear constraints, subject to the "Variable Bounds" gap above.
- **Solution Extraction**: Working. Solutions are correctly mapped back to output rows.

## Audit Findings (Potential Issues)

### 1. Usability: Minus Operator in SUM
- **Issue**: `SUM(x - y)` or `SUM(x - 5)` is rejected by `ValidateSumArgumentInternal`. Users must write `SUM(x + (-1 * y))` or `SUM(x + (-5))`.
- **Severity**: Medium (Usability)
- **Location**: `src/planner/expression_binder/decide_binder.cpp`

### 2. Symbolic Normalization Gap
- **Issue**: `NormalizeComparisonExpr` in `decide_symbolic.cpp` explicitly ignores `COMPARE_EQUAL`. While `PhysicalDecide` handles basic equality, complex equality constraints (e.g., `SUM(x) + 5 = 10`) might not be normalized correctly, potentially leading to binder or solver errors for complex expressions.
- **Severity**: Low (Potential robustness issue)
- **Location**: `src/packdb/symbolic/decide_symbolic.cpp`

### 3. Variable Type Restriction
- **Issue**: `PhysicalDecide` explicitly throws an error if a DECIDE variable is `DOUBLE` or `FLOAT`. It enforces `INTEGER` or `BINARY`. Dead code exists in `GetData` for `DOUBLE` extraction.
- **Severity**: Low (Design choice, but limits scope)
- **Location**: `src/execution/operator/decide/physical_decide.cpp`

### 4. Subquery Fragility
- **Issue**: `DecideBinder` executes subqueries by re-parsing the SQL string (`expr.ToString()`). This is fragile and will fail confusingly if the subquery text is not a valid standalone query (e.g., relies on outer scope, even if uncorrelated in logic).
- **Severity**: Low
- **Location**: `src/planner/expression_binder/decide_binder.cpp`
