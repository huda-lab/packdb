# Implementation Part 2: Binder & Validation

## 1. Overview

After parsing, the `DECIDE` clause needs semantic validation. The Binder is responsible for ensuring that the user's query makes sense in the context of the database schema and the mathematical constraints of the solver.

**Key Source Files**:

- `src/planner/expression_binder/decide_binder.cpp`
- `src/planner/expression_binder/decide_constraints_binder.cpp`

## 2. Variable Scope & Binding

Unlike a standard `GROUP BY` or `SELECT` clause, the `DECIDE` clause introduces variables that do not exist in any physical table.

- **Decide Variables**: These are "virtual" columns representing the decision.
- **Binding Context**: The binder creates a special scope where these variables are valid. It verifies that variable names do not collide with existing table columns.

## 3. Constraint Validation

The primary role of the `DecideConstraintsBinder` is to enforce the **Linearity Assumption** required by ILP solvers.

### 3.1 Linearity Check

Every term on the LHS of a constraint must optionally involve a Decide Variable, but never in a non-linear way.

- **Valid**: `2*x`, `x`, `price * x` (assuming `price` is a table column, constant for the decision).
- **Invalid**: `x * x` (Quadratic), `x * y` (Interaction), `SIN(x)`.

The binder walks the expression tree and flags an error if it encounters a multiplication between two sub-trees that both contain decision variables.

### 3.2 Subquery Handling

PackDB supports both **uncorrelated and correlated scalar subqueries** in constraints.

- Example (uncorrelated): `SUM(x) <= (SELECT COUNT(*) FROM Drivers)`
- Example (correlated): `x <= (SELECT budget FROM Depts WHERE Depts.id = items.dept_id)`
- **Mechanism**: Subqueries are delegated to DuckDB's standard `ExpressionBinder::BindExpression`, which handles both cases via `PlanSubqueries`:
  - **Uncorrelated**: Evaluated once as cross-joined scalars (constant RHS).
  - **Correlated**: Decorrelated into joins, producing per-row values.
- **Validation**:
  - Only scalar subqueries are supported (non-scalar returns an error).
  - Subqueries cannot reference DECIDE variables (checked via `ExpressionContainsDecideVariable` at `decide_binder.cpp:246`).
  - For aggregate constraints (`SUM`, `AVG`), the RHS must be a scalar (same value for all rows). This is validated at execution time in `ilp_model_builder.cpp` — if the correlated subquery produces different values per row, an error is thrown.

### 3.3 Operator Restrictions

- **IN operator**: `IN` on decision variables is supported via rewrite to K auxiliary binary indicator variables (one per value in the set), with cardinality and linking constraints. See Section 6.1.
- **Standard comparisons** (`=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`): Supported on both per-row and aggregate constraints.

## 4. Type Inference & Syntactic Sugar

Type declarations are specified in the `DECIDE` clause itself (e.g., `DECIDE x IS BOOLEAN`). The binder translates these into the appropriate internal representation and adds implicit constraints.

| DECIDE Syntax         | Internal Representation | Added Constraints     |
| :-------------------- | :---------------------- | :-------------------- |
| `DECIDE x IS INTEGER` | `x` (Type: Integer)     | `x >= 0`              |
| `DECIDE x IS BOOLEAN` | `x` (Type: Integer)     | `x >= 0` AND `x <= 1` |
| `DECIDE x`            | `x` (Type: Integer)     | `x >= 0` (default)    |
| `DECIDE x IS REAL`    | `x` (Type: Double)      | `x >= 0`              |

Note: DuckDB's internal `LogicalType::INTEGER` is used for INTEGER and BOOLEAN decision variables. `IS BOOLEAN` is strictly a domain constraint, not a storage type. `IS REAL` variables use `LogicalType::DOUBLE` internally and generate continuous (non-integer) solver variables.

## 5. `WHEN` Condition Validation

### 5.1 WHEN on Constraints

The `DecideConstraintsBinder` handles WHEN constraints via `BindWhenConstraint`:

1. **Validation**: The WHEN condition (child[1]) is checked with `ExpressionContainsDecideVariable` — it must reference only table columns, not decision variables.
2. **Constraint binding**: The constraint (child[0]) is bound through normal DECIDE constraint dispatch (linearity validation, etc.).
3. **Condition binding**: The condition (child[1]) is bound using the base `ExpressionBinder` (via `binding_when_condition` flag), bypassing DECIDE-specific validation since the condition is a data filter, not an optimization constraint.
4. **Output**: A tagged `BoundConjunctionExpression` with `alias = WHEN_CONSTRAINT_TAG` carries both the bound constraint and bound condition downstream to the execution layer.

### 5.2 WHEN on Objective

The `DecideObjectiveBinder` handles WHEN on the objective (`MAXIMIZE SUM(...) WHEN condition`) using the same pattern:

1. **Detection**: The binder checks for a `FunctionExpression` with `function_name == WHEN_CONSTRAINT_TAG` at the top level.
2. **Validation**: The WHEN condition must not reference decision variables.
3. **Objective binding**: The objective (child[0]) is bound through normal objective binding (SUM validation, linearity check).
4. **Condition binding**: The condition (child[1]) is bound via `ExpressionBinder` using the `binding_when_condition` flag bypass.
5. **Output**: A tagged `BoundConjunctionExpression` with `alias = WHEN_CONSTRAINT_TAG`, identical in structure to the constraint WHEN output.

### 5.3 PER Constraint Validation

- PER expressions must reference a table column (not a decision variable, not a constant).
- PER is only valid on aggregate constraints (constraints using SUM/COUNT/AVG).
- The PER column creates one constraint per distinct value of that column.
- Combined WHEN+PER: WHEN filters rows first, then PER groups the remaining rows.

## 6. Rewrite Passes (Proto-Optimizer)

These algebraic rewrites live in `bind_select_node.cpp` and are applied during binding. They are **optimizer candidates** — algebraic rewrites that logically belong in a dedicated optimizer pass but currently live in the binder for simplicity.

### 6.1 COUNT → SUM Rewrite

- **BOOLEAN variables**: `COUNT(x)` is directly rewritten to `SUM(x)` since a BOOLEAN var is 0 or 1, so counting non-zero = summing.
- **INTEGER variables**: Creates an indicator variable `__count_ind_VAR__` (BOOLEAN), rewrites `COUNT(x)` → `SUM(indicator)`. At execution time, Big-M linking constraints are generated: `x <= M * indicator` and `x >= indicator` (ensuring indicator=1 iff x>0).
- **REAL variables**: Not yet supported; throws an explicit error.
- **Code**: `RewriteCountToSum()` in `bind_select_node.cpp` (lines ~413-466)

### 6.2 AVG → SUM Rewrite

- `AVG(expr)` is rewritten to `SUM(expr)` with a special tag (`AVG_REWRITE_TAG` alias).
- At execution time, the model builder detects this tag and scales the RHS by the number of rows in each group, effectively converting `AVG(expr) <= K` into `SUM(expr) <= K * N`.
- **Code**: `RewriteAvgToSum()` in `bind_select_node.cpp` (lines ~490-505)

### 6.3 ABS Linearization

- `ABS(expr)` where `expr` contains decision variables is linearized using a standard LP technique:
  - Creates an auxiliary REAL variable `__abs_aux_N__`
  - Generates two linearization constraints: `aux >= expr` and `aux >= -expr`
  - Replaces `ABS(expr)` with `aux` in the original expression
- This works because minimizing `aux` subject to `aux >= expr` and `aux >= -expr` forces `aux = |expr|`.
- **Code**: `RewriteAbsInExpression()` / `RewriteAbsLinearization()` in `bind_select_node.cpp` (lines ~510-591)

> **Note**: All three rewrites are tagged as **optimizer candidates** — they perform algebraic transformations, not semantic validation, and should eventually migrate to a `LogicalDecideOptimizer` pass.

## 7. Auxiliary Variable Management

When rewrite passes create auxiliary variables (COUNT indicators, ABS auxiliary vars), they are tracked on the `BoundSelectNode` and carried forward to `LogicalDecide` / `PhysicalDecide`:

- **`num_auxiliary_vars`**: Count of auxiliary variables appended after user-declared variables.
- **`count_indicator_links`**: Vector of `(indicator_var_index, original_var_index)` pairs used at execution time to generate Big-M linking constraints.
- **Hiding from SELECT ***: Auxiliary variables are pruned from the bind context (lines ~748-758 of `bind_select_node.cpp`) so they don't appear in query results. They exist only in the solver's variable space.
