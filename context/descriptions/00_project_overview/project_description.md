# PackDB: Package Query Extension for DuckDB

## 1. Introduction

### 1.1 Problem Statement
Standard Database Management Systems (DBMS) are excellent at retrieving and aggregating data but lack native support for combinatorial optimization problems. Users often need to select a subset of items (a "package") that satisfies various constraints while optimizing an objective function (e.g., "Select a meal plan with max protein but under 2000 calories" or "Choose a team of engineers within budget"). Typically, this requires extracting data to an external solver, which is inefficient and complex to maintain.

### 1.2 Solution: PackDB
PackDB extends DuckDB with native support for **Package Queries**. It introduces a declarative `DECIDE` clause to SQL, allowing users to express these optimization problems directly within the query language. PackDB handles the translation of these high-level requirements into an Integer Linear Programming (ILP) model, solves it using an embedded solver (HiGHS), and returns the optimal package as standard relational tables.

## 2. Project Goals

1.  **Seamless Integration**: Extend SQL syntax naturally without breaking existing functionality.
2.  **Performance**: Execute optimization queries efficiently by pushing constraints down and solving directly within the database engine.
3.  **Usability**: Abstract away the complexities of mathematical modeling (ILP, solvers matrices) from the database user.

## 3. The `DECIDE` Clause

The core contribution is the new SQL syntax:

```sql
SELECT select_list
FROM table_expression
[WHERE ...]
DECIDE variable [IS type] [, variable2 [IS type2] ...]
SUCH THAT constraint_list
[MAXIMIZE | MINIMIZE] objective_function
```

### 3.1 Key Components
-   **Decision Variables**: `DECIDE x IS INTEGER, y IS BOOLEAN` defines variables with their types representing the quantity or selection status of rows.
-   **Variable Types**: Specified in the DECIDE clause. `IS INTEGER` (default) or `IS BOOLEAN` enforces the domain.
-   **Constraints**: `SUM(x * cost) <= Budget` or `x <= 1` defines the bounds.
-   **Objective**: `MAXIMIZE SUM(x * utility)` defines the goal.

## 4. System Overview
PackDB modifies the standard query processing pipeline:
1.  **Parser**: Recognizes the new keywords and builds a symbolic representation.
2.  **Binder**: Validates variable scopes, types, and mathematical properties (linearity).
3.  **Planner**: Inserts a `LogicalDecide` operator into the query plan.
4.  **Execution**: The `PhysicalDecide` operator materializes data, formulates the ILP model, invokes the HiGHS solver, and maps results back to the query output.

*For detailed documentation of each pipeline stage, see `../01_pipeline/`.*