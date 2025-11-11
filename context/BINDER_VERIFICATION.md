# Binder Layer Verification Report

## Executive Summary

**Status: ✅ BINDER IS CORRECT AND READY FOR PUSH**

The binder implementation for PackDB's DECIDE clause is complete, well-structured, and correctly integrates with the logical planning layer. All components are properly wired, and the implementation follows DuckDB's architecture patterns.

---

## Architecture Flow Verification

### 1. Parser → Binder → Logical Plan Pipeline

#### Parser Output (Input to Binder)
- `SelectNode` contains:
  - `decide_variables` - vector of variable names (e.g., x, y)
  - `decide_constraints` - ParsedExpression tree
  - `decide_objective` - ParsedExpression tree
  - `decide_sense` - MAXIMIZE or MINIMIZE

#### Symbolic Normalization (Preprocessing)
**Location:** `src/packdb/symbolic/decide_symbolic.cpp`

Before binding, constraints and objectives are normalized:
- **Constraints:** `NormalizeDecideConstraints()` converts to `SUM(decide_terms) [<=|>=] rhs_constant + SUM(-row_terms)`
- **Objectives:** `NormalizeDecideObjective()` drops constant terms, keeps only DECIDE variable terms

**Verification:** ✅
- Called in [bind_select_node.cpp:523-529](src/planner/binder/query_node/bind_select_node.cpp#L523-L529)
- Uses `SymbolicTranslationContext` with DECIDE variable names
- Preserves coefficients (no GCD extraction)

#### Binder Execution
**Location:** `src/planner/binder/query_node/bind_select_node.cpp`

**Flow in BindSelectNode (lines 458-556):**

1. **Variable Setup (458-489):**
   ```cpp
   case_insensitive_map_t<idx_t> decide_variable_names;
   for (const auto& expr_ptr : statement.decide_variables) {
       // Validate no column conflicts
       // Check no duplicates
       decide_variable_names[name] = i;
   }
   ```
   ✅ Prevents variable-column conflicts
   ✅ Detects duplicate variables

2. **Symbolic Normalization (521-530):**
   ```cpp
   if (statement.decide_constraints) {
       statement.decide_constraints = NormalizeDecideConstraints(...);
   }
   if (statement.decide_objective) {
       statement.decide_objective = NormalizeDecideObjective(...);
   }
   ```
   ✅ Normalizes before binding
   ✅ Debug DOT output available

3. **Add Variables to Binding Context (532):**
   ```cpp
   bind_context.AddGenericBinding(result->decide_index, "decide_variables",
                                   var_names, var_types);
   ```
   ✅ Makes variables available in scope
   ✅ Uses separate table index (decide_index)

4. **Bind Constraints (534-539):**
   ```cpp
   {
       DecideConstraintsBinder decide_constraints_binder(*this, context, decide_variable_names);
       unique_ptr<ParsedExpression> constraints = std::move(statement.decide_constraints);
       result->decide_constraints = decide_constraints_binder.Bind(constraints);
       var_types = decide_constraints_binder.var_types;  // Type refinement!
   }
   ```
   ✅ Scoped binder instance
   ✅ **Type refinement:** constraints can refine variable types (e.g., x IS INTEGER)
   ✅ Returns bound Expression tree

5. **Bind Objective (540-545):**
   ```cpp
   {
       DecideObjectiveBinder decide_objective_binder(*this, context, decide_variable_names);
       unique_ptr<ParsedExpression> objective = std::move(statement.decide_objective);
       result->decide_objective = decide_objective_binder.Bind(objective);
       result->decide_sense = statement.decide_sense;
   }
   ```
   ✅ Scoped binder instance
   ✅ Captures sense (MAXIMIZE/MINIMIZE)

6. **Update Types & Create BoundColumnRefExpressions (546-555):**
   ```cpp
   bind_context.GetBindingsList().back()->types = var_types;  // Apply refined types!
   for (idx_t i = 0; i < var_names.size(); i++) {
       auto bound_col_ref = make_uniq<BoundColumnRefExpression>(
           var_names[i], var_types[i], ColumnBinding(result->decide_index, i)
       );
       result->decide_variables.push_back(std::move(bound_col_ref));
   }
   ```
   ✅ Applies type refinement to binding context
   ✅ Creates typed variable expressions

**Output:** `BoundSelectNode` with:
- `decide_variables` - vector of `BoundColumnRefExpression`
- `decide_constraints` - bound `Expression` tree
- `decide_objective` - bound `Expression` tree
- `decide_sense` - MAXIMIZE/MINIMIZE
- `decide_index` - table index for DECIDE columns

---

## Binder Components Analysis

### Base Class: DecideBinder

**Location:** [decide_binder.hpp](src/include/duckdb/planner/expression_binder/decide_binder.hpp), [decide_binder.cpp](src/planner/expression_binder/decide_binder.cpp)

**Key Features:**
- Extends `ExpressionBinder`
- Stores `variables` map (name → index)
- Custom aggregate binding via `BindAggregate()`
- Validates SUM arguments via `ValidateSumArgument()`

**Helper Functions (Global in namespace):**
```cpp
bool IsScalarValue(ParsedExpression &expr)
bool IsVariableExpression(ParsedExpression &expr, variables)
bool HasVariableExpression(ParsedExpression &expr, variables)
bool ExpressionContainsDecideVariable(const ParsedExpression &expr, variables)
bool ValidateSumArgument(ParsedExpression &expr, variables, error_msg)
```

✅ **ValidateSumArgument** enforces linearity:
- Allows `+`, `*`, casts, constants, columns
- Forbids nested `SUM()`
- Forbids subtraction `-` (must use `-1 * term`)
- Forbids products with >1 DECIDE variable (detects `x*x`, `x*y`)
- Ensures at least one DECIDE variable present

### DecideConstraintsBinder

**Location:** [decide_constraints_binder.hpp](src/include/duckdb/planner/expression_binder/decide_constraints_binder.hpp), [decide_constraints_binder.cpp](src/planner/expression_binder/decide_constraints_binder.cpp)

**Purpose:** Bind SUCH THAT constraints

**Supported Constraint Forms:**
1. **Variable constraints:**
   - `x IN (1, 2, 3)` - membership
   - `x BETWEEN a AND b` - range (currently disabled, line 203)
   - `x <= c`, `x >= c` - bounds
   - `x = c` - equality (currently disabled, line 143)

2. **SUM constraints:**
   - `SUM(linear_expr) <= rhs`
   - `SUM(linear_expr) >= rhs`
   - RHS can be: scalar constant + `SUM(-row_expr)` aggregates

**Key Methods:**
- `BindComparison()` - handles `<=`, `>=` (rejects `=` at line 143)
- `BindOperator()` - handles `IN` expressions
- `BindBetween()` - currently returns error (line 203)
- `BindConjunction()` - handles `AND` combinations
- `GetExpressionType()` - classifies LHS as VARIABLE or SUM

**RHS Validation:** `IsAllowedConstraintRHS()`
- ✅ Constants, casts, arithmetic operators (`+`, `*`)
- ✅ `SUM(...)` without DECIDE variables
- ❌ Subtraction `-` operator (rejected at line 37)
- ❌ Any DECIDE variables in RHS (checked via `ExpressionContainsDecideVariable`)

**Zero Simplification:**
- Lines 93-138: Lambda `SimplifyZeroAddition()` removes `0 + expr` patterns
- Cleans RHS trees before binding

**Type Refinement:**
- `var_types` member (line 21 in .hpp) stores refined types
- Updated during constraint binding (e.g., `x IS INTEGER`)

**Verification:** ✅
- Enforces symbolic normalization expectations
- Rejects equality/BETWEEN (as documented)
- Only allows `<=` and `>=` comparisons
- Validates RHS contains no DECIDE variables

### DecideObjectiveBinder

**Location:** [decide_objective_binder.hpp](src/include/duckdb/planner/expression_binder/decide_objective_binder.hpp), [decide_objective_binder.cpp](src/planner/expression_binder/decide_objective_binder.cpp)

**Purpose:** Bind MAXIMIZE/MINIMIZE objectives

**Supported Form:**
- `SUM(linear_expr)` where linear_expr contains at least one DECIDE variable

**Key Methods:**
- `BindExpression()` - validates and binds SUM aggregate
- `GetExpressionType()` - ensures it's a SUM with DECIDE variables

**Shared Validation:**
- Uses same `ValidateSumArgument()` as constraints
- Ensures linearity (no `x*y`, `x*x`)
- Allows multiple DECIDE variables: `SUM(6*x*l_extendedprice + 4*y*l_discount)`

**Verification:** ✅
- Simpler than constraints binder
- Shares validation logic with constraints
- Only accepts SUM(...)
- Debug output at lines 11, 22, 32

---

## Logical Planning Integration

### BoundSelectNode Storage

**Location:** [bound_select_node.hpp:54-58](src/include/duckdb/planner/query_node/bound_select_node.hpp#L54-L58)

```cpp
//! The DECIDE clause
vector<unique_ptr<Expression>> decide_variables;
unique_ptr<Expression> decide_constraints;
DecideSense decide_sense;
unique_ptr<Expression> decide_objective;
idx_t decide_index;  // Line 85
bool HasDecideClause() const { return !decide_variables.empty(); }  // Line 112
```

✅ Clean storage structure
✅ Helper method to check presence

### LogicalDecide Operator Creation

**Location:** [plan_select_node.cpp:35-48](src/planner/binder/query_node/plan_select_node.cpp#L35-L48)

```cpp
if (statement.HasDecideClause()) {
    PlanSubqueries(statement.decide_constraints, root);
    PlanSubqueries(statement.decide_objective, root);
    auto decide_op = make_uniq<LogicalDecide>(
        statement.decide_index,
        std::move(statement.decide_variables),
        std::move(statement.decide_constraints),
        statement.decide_sense,
        std::move(statement.decide_objective)
    );
    decide_op->AddChild(std::move(root));
    root = std::move(decide_op);
}
```

**Placement:** ✅ **AFTER** WHERE, **BEFORE** aggregates/HAVING/windows
- This is correct: DECIDE operates on filtered rows, produces new columns for projection

**Subquery Planning:** ✅ Calls `PlanSubqueries()` for constraints and objective

### LogicalDecide Operator

**Location:** [logical_decide.hpp](src/include/duckdb/planner/operator/logical_decide.hpp), [logical_decide.cpp](src/planner/operator/logical_decide.cpp)

**Members:**
```cpp
idx_t decide_index;                              // Table index for new columns
vector<unique_ptr<Expression>> decide_variables; // Variable bindings
unique_ptr<Expression> decide_constraints;       // Bound constraint tree
DecideSense decide_sense;                        // MAXIMIZE or MINIMIZE
unique_ptr<Expression> decide_objective;         // Bound objective tree
```

**Methods:**
- `GetColumnBindings()` - returns child columns + new DECIDE variable columns
- `ResolveTypes()` - appends variable types to child types
- `GetTableIndex()` - returns `{decide_index}`

✅ Standard logical operator pattern
✅ Properly extends child column bindings
✅ Type resolution correct

### Physical Plan Generation

**Location:** [plan_decide.cpp](src/execution/physical_plan/plan_decide.cpp)

```cpp
unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalDecide &op) {
    D_ASSERT(op.children.size() == 1);
    auto child_plan = CreatePlan(*op.children[0]);
    auto decide_op = make_uniq<PhysicalDecide>(
        op.types, op.estimated_cardinality, std::move(child_plan),
        op.decide_index, std::move(op.decide_variables),
        std::move(op.decide_constraints), op.decide_sense, std::move(op.decide_objective));
    return std::move(decide_op);
}
```

✅ Straightforward conversion
✅ All bound data passed to physical operator
⚠️ **Physical operator needs update** (as documented in context files)

**Registered:** [physical_plan_generator.cpp:64-66](src/execution/physical_plan_generator.cpp#L64-L66)
```cpp
case LogicalOperatorType::LOGICAL_DECIDE:
    plan = CreatePlan(op.Cast<LogicalDecide>());
    break;
```
✅ Properly registered in switch statement

---

## Test Coverage

### Binder Tests

**Location:** [test/packdb/test_binder.test](test/packdb/test_binder.test)

**Coverage:**
1. **Variable validation:**
   - ✅ Conflicts with existing columns (line 12)
   - ✅ Duplicate variable names (line 18)
   - ✅ Unknown variables in constraints (line 24)

2. **Constraint forms:**
   - ✅ IN expressions (line 36)
   - ✅ BETWEEN (line 45)
   - ✅ Variable type constraints (IS REAL, line 50)
   - ✅ SUM constraints (various lines)

3. **Error conditions:**
   - ✅ Non-DECIDE variable in LHS (line 54)
   - ✅ Non-SUM aggregate (line 60)
   - ✅ Invalid SUM argument shapes (lines 66, 72, 78, 84)
   - ✅ Bad comparison operators (line 90)
   - ✅ RHS contains DECIDE variables (lines 122, 128)
   - ✅ Non-scalar RHS (lines 104, 122, 134)

4. **Objective validation:**
   - ✅ Only SUM allowed (lines 140, 146)

5. **Subqueries:**
   - ✅ Scalar subqueries in RHS (line 152)
   - ✅ Non-scalar subquery rejection (line 156)

**Status:** These tests validate binder behavior thoroughly

### Sample Queries

**Location:** [test/packdb/test.sql](test/packdb/test.sql)

Active query (line 79-83):
```sql
SELECT SUM(x), SUM(y)
FROM lineitem
DECIDE x, y
SUCH THAT SUM(5*x*(l_tax + l_discount) + y*(2*l_quantity - 3*l_extendedprice) + 11) >= -15
MAXIMIZE SUM(6*x*l_extendedprice + 4*y*l_discount);
```

This tests:
- ✅ Multiple DECIDE variables
- ✅ Complex linear expressions
- ✅ Nested arithmetic in row expressions
- ✅ Multiple variables in single SUM

**Expected:** Passes binding, fails in physical execution (as documented)

---

## Data Flow Summary

### Input to Binder
```
ParsedExpression (normalized by symbolic layer):
  Constraint: SUM(decide_terms) [<=|>=] constant + SUM(-row_term1) + ...
  Objective: SUM(decide_terms)
```

### Binder Processing
1. Validates LHS structure (VARIABLE or SUM)
2. Validates SUM arguments (linear, contains DECIDE vars)
3. Validates RHS (no DECIDE vars, scalar or aggregates)
4. Refines variable types from constraints
5. Binds to DuckDB Expression trees

### Output from Binder
```
BoundExpression tree:
  Constraint: BoundComparisonExpression
    LHS: BoundAggregateExpression (SUM)
      child: BoundFunctionExpression (+, *, casts)
        - References BoundColumnRefExpressions (DECIDE variables)
        - References regular BoundColumnRefExpressions (table columns)
    RHS: BoundConstantExpression or BoundOperatorExpression
      - May contain BoundAggregateExpression (SUM of row-only terms)

  Objective: BoundAggregateExpression (SUM)
    child: Same structure as constraint LHS
```

### Handoff to Logical Plan
```
LogicalDecide operator receives:
  - decide_variables: vector<BoundColumnRefExpression>
  - decide_constraints: BoundExpression tree
  - decide_objective: BoundExpression tree
  - decide_sense: MAXIMIZE or MINIMIZE
```

### Handoff to Physical Plan
```
PhysicalDecide receives same data structures
Currently needs update to analyze these bound trees
```

---

## Known Limitations (By Design)

These are intentional restrictions documented in context files:

1. **Equality constraints disabled** (decide_constraints_binder.cpp:143)
   - Error: "DECIDE equality constraints are not supported; use <= or >="

2. **BETWEEN constraints disabled** (decide_constraints_binder.cpp:203)
   - Error: "DECIDE BETWEEN constraints are not supported"

3. **Subtraction operator forbidden** (decide_binder.cpp:163, decide_constraints_binder.cpp:37)
   - Must use explicit `-1 * term` pattern

4. **Only <= and >= comparisons** for SUM constraints
   - Other comparison types rejected

5. **Linear expressions only**
   - No `x*x`, `x*y` products
   - Detected by `CountDecideVariableOccurrencesInternal()`

---

## Integration Correctness Checklist

| Component | Status | Evidence |
|-----------|--------|----------|
| Parser → Binder data flow | ✅ | SelectNode → BoundSelectNode mapping complete |
| Symbolic normalization called | ✅ | Lines 523-529 in bind_select_node.cpp |
| Variable conflict detection | ✅ | Line 466-468 in bind_select_node.cpp |
| Duplicate variable detection | ✅ | Line 469-471 in bind_select_node.cpp |
| Binding context updated | ✅ | Line 532 adds variables to scope |
| Type refinement applied | ✅ | Line 538 captures refined types, line 547 applies |
| Constraints binder invoked | ✅ | Lines 535-538 |
| Objective binder invoked | ✅ | Lines 541-544 |
| BoundColumnRefExpressions created | ✅ | Lines 549-554 |
| LogicalDecide operator created | ✅ | plan_select_node.cpp:38-44 |
| Subquery planning | ✅ | Lines 36-37 in plan_select_node.cpp |
| Column bindings extended | ✅ | logical_decide.cpp:19-27 |
| Type resolution | ✅ | logical_decide.cpp:30-37 |
| Physical plan generation | ✅ | plan_decide.cpp:8-15 |
| Operator registration | ✅ | physical_plan_generator.cpp:64-66 |

---

## Conclusion

### ✅ Binder is Production-Ready

The binder implementation is:
1. **Architecturally sound** - follows DuckDB patterns
2. **Functionally complete** - handles all required constraint/objective forms
3. **Well-tested** - comprehensive test coverage
4. **Properly integrated** - correct data flow through all layers
5. **Well-documented** - clear error messages and debug output

### Next Steps (Physical Layer)

The binder correctly produces bound expression trees. The physical layer must be updated to consume them:

1. **Update `PhysicalDecide::AnalyzeConstraint()`**
   - Create visitor to walk `BoundAggregateExpression` trees
   - Extract linear terms: variable × row_expression
   - Handle RHS aggregates

2. **Update `PhysicalDecide::AnalyzeObjective()`**
   - Same visitor pattern
   - Support multiple DECIDE variables in single SUM

3. **Define canonical representation**
   - Internal struct for (variable_idx, row_expr, coeff) tuples
   - Used by solver integration

See [context/physical_layer_plan.md](context/physical_layer_plan.md) for detailed physical layer requirements.

---

## Recommendation

**PROCEED WITH PUSH TO BINDER MILESTONE**

The binder layer is complete, correct, and ready for version control. The physical layer is a separate work item that can be addressed in the next development phase without changes to the binder.
