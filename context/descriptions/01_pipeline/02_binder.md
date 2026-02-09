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

PackDB supports **uncorrelated scalar subqueries** in the bounds of constraints.

- Example: `SUM(x) <= (SELECT COUNT(*) FROM Drivers)`
- **Mechanism**: These subqueries are executed immediately during the Binding phase. The result is replaced by a constant value in the bound, ensuring the Solver receives a static problem definition.

## 4. Type Inference & Syntactic Sugar

Type declarations are specified in the `DECIDE` clause itself (e.g., `DECIDE x IS BOOLEAN`). The binder translates these into the appropriate internal representation and adds implicit constraints.

| DECIDE Syntax         | Internal Representation | Added Constraints     |
| :-------------------- | :---------------------- | :-------------------- |
| `DECIDE x IS INTEGER` | `x` (Type: Integer)     | `x >= 0`              |
| `DECIDE x IS BOOLEAN` | `x` (Type: Integer)     | `x >= 0` AND `x <= 1` |
| `DECIDE x`            | `x` (Type: Integer)     | `x >= 0` (default)    |

Note: DuckDB's internal `LogicalType::INTEGER` is used for all decision variables. `IS BOOLEAN` is strictly a domain constraint, not a storage type.
