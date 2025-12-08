This repo basically extends duckdb to handle package queries. We are adding the following sytax for it:

SELECT *
FROM Recipes R
WHERE R.gluten_free = ’TRUE’
DECIDE x
SUCH THAT x IS INTEGER AND
    x BETWEEN 0 AND 4
    SUM(x) = 7 AND
    SUM(calories*x) BETWEEN 2000 AND 2500
MAXIMIZE SUM(protein*x)

Decide variables are the variables that are decided by the query. They represent the cardinality of each item in the final package.

### Project summary

PackDB extends DuckDB with package queries via a new DECIDE/SUCH THAT/[MAXIMIZE|MINIMIZE] clause, enabling optimization over per-row decision variables.

- **Syntax**
  - **DECIDE variables**: define one or more decision variables (e.g., `DECIDE x, y`). Types via `SUCH THAT x IS [REAL|INTEGER|BINARY]`.
  - **Constraints (SUCH THAT)**: left-hand side must be a DECIDE variable or `SUM(...)` over a DECIDE variable. Supported forms: `x IN (...)`, `x BETWEEN a AND b`, `x <= c`, `x >= c`, `x = c`, and `SUM(x * expr) [=|<=|>=] scalar`. Right-hand sides must be scalar and contain no DECIDE variables.
  - **Objective**: `[MAXIMIZE|MINIMIZE] SUM(x * expr)`; only `SUM` is allowed and must involve a DECIDE variable.

- **Parser**
  - `SelectNode` extended with `decide_variables`, `decide_constraints`, `decide_sense`, `decide_objective`.
  - Transformer populates these from `DECIDE ... SUCH THAT ... [MAXIMIZE|MINIMIZE] ...`.

- **Binder**
  - Validates variable names (no column conflicts, no duplicates) and creates a binding for decide variables.
  - `DecideConstraintsBinder` enforces allowed constraint shapes and refines variable types (e.g., INTEGER).
  - `DecideObjectiveBinder` restricts objective to `SUM(...)` over DECIDE variables and records sense.

- **Planner**
  - Inserts `LogicalDecide` above the regular plan carrying variables, constraints, sense, and objective.

- **Execution**
  - `PhysicalDecide` collects child rows, analyzes constraints/objective, and integrates with the HiGHS solver to compute optimal solutions.
  - Supports bind-time execution of uncorrelated scalar subqueries.

- **Enums & operators**
  - New enums: `DecideSense`, `DecideExpression`, `DeterministicConstraintSense`.
  - Operators: `LogicalDecide`, `PhysicalDecide`.


### Current status and next steps

- Implemented: Parsing, Binding, Logical Planning, Physical Execution, Solver Integration.
- Features: Variable bounds, Equality constraints, BETWEEN constraints, Subquery support.