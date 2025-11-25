# Pipeline Issues & Critical Gaps

## Critical Blockers (Must Fix)

### 1. Variable Bounds and Types Extraction (Phase 0 Gap)
**Severity**: High
**Location**: `src/execution/operator/decide/physical_decide.cpp` (Lines 656-661)

Currently, the Physical Layer **hardcodes** all DECIDE variables to:
- **Type**: `INTEGER`
- **Bounds**: `[0, 4]`

This means any query specifying `x IS BINARY` or `x <= 100` will be **ignored** by the solver, leading to incorrect results or infeasibility.

**Required Fix**:
1. **Extract Types**: Read `decide_variables[i]->return_type` to determine if a variable is `INTEGER`, `BINARY`, or `REAL`.
2. **Extract Bounds**: Traverse `decide_constraints` to find variable-level bounds (e.g., `x <= 10`, `x >= 5`) and apply them to the HiGHS model.

## Missing Features

### 2. Equality Constraints
**Severity**: Medium
**Location**: `src/planner/expression_binder/decide_constraints_binder.cpp`

The Binder currently **rejects** equality constraints (e.g., `SUM(x) = 10`) with a "not supported" error.
- **Status**: The Physical Layer *can* handle equalities (it sets lower_bound = upper_bound), but the Binder blocks them.
- **Fix**: Update `DecideConstraintsBinder` to allow `COMPARE_EQUAL`.

### 3. BETWEEN Constraints
**Severity**: Medium
**Location**: `src/planner/expression_binder/decide_constraints_binder.cpp`

The Binder currently **rejects** `BETWEEN` constraints (e.g., `SUM(x) BETWEEN 10 AND 20`).
- **Status**: The Symbolic layer supports `BETWEEN`, but the Binder blocks it.
- **Fix**: Update `DecideConstraintsBinder` to validate and accept `BETWEEN` expressions.

### 4. Subquery Support
**Severity**: Low
**Location**: Binder & Physical Layer

Subqueries are currently limited to the RHS of constraints (scalar subqueries). Support for subqueries in objective coefficients or LHS terms is limited or untested.

## Solver Integration Status
- **HiGHS Integration**: Working. The solver is correctly linked and called.
- **Model Building**: Working for basic linear constraints, subject to the "Variable Bounds" gap above.
- **Solution Extraction**: Working. Solutions are correctly mapped back to output rows.
