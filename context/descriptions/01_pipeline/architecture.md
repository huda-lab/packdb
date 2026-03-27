# System Architecture

## 1. High-Level Design

PackDB is implemented as an extension to DuckDB. It leverages DuckDB's extensible parser and planner architecture to inject new query operators. The system follows a pipelined execution model where the `DECIDE` clause is treated as a specialized aggregation step that occurs after standard filtering and before final projection.

### 1.1 Core Components

*   **DuckDB Core**: Responsible for storage, transaction management, and executing the standard relational parts of the query (scans, joins, filters).
*   **Symbolic Layer (Parser)**: Uses `SymbolicC++` to parse and normalize algebraic expressions within the `DECIDE` and `SUCH THAT` clauses. It converts user-friendly SQL expressions into a canonical form.
*   **Binder & Rewrite Passes**: Validates the mathematical properties of the optimization problem (e.g., linearity checks) and applies algebraic rewrites (COUNT→SUM, AVG→SUM, ABS linearization) that prepare expressions for the solver.
*   **Execution Runtime**: A dedicated physical operator (`PhysicalDecide`) that acts as a bridge between the relational engine and the linear solver.
*   **Solvers**: PackDB uses **Gurobi** as its primary ILP solver — empirical benchmarking showed it to be significantly faster than HiGHS for PackDB workloads. **HiGHS** (open-source, bundled) is retained as a fallback for environments without a Gurobi license, but is substantially slower in practice.

## 2. Query Lifecycle

The execution of a Package Query follows these stages:

```mermaid
graph TD
    User[User SQL Query] --> Parser
    subgraph DuckDB + PackDB Extension
    Parser[Parser (DuckDB + Symbolic Layer)] --> Binder
    Binder[Binder (Validation & Types)] --> Planner
    Planner[Logical Planner] --> Opt[Optimizer]
    Opt --> Phys[Physical Planner]
    Phys --> Exec[Execution Engine]
    
    subgraph PackDB Execution
        Exec --> Data[Materialize Candidates]
        Data --> Matrix[Build Solver Matrix]
        Matrix --> Model[ILPModel Builder]
        Model --> Solver[Solver: Gurobi (primary) / HiGHS (slow fallback)]
        Solver --> Result[Map Solution to Rows]
    end
    end
    Result --> Output[Result Table]
```

## 3. Detailed Data Flow

### 3.1 Parsing & Normalization
When the parser encounters a `DECIDE` clause, it invokes the Symbolic Layer. This layer:
1.  Identifies decision variables.
2.  Normalizes constraints into the form `SUM(coeff * variable) <= constant`.
3.  Separates row-varying coefficients (dependent on table columns) from decision variables.

### 3.2 Plan Generation
The planner inserts a `LogicalDecide` operator into the query tree. Crucially, this operator is placed **above** the `VideoTable` (or any source table) and `Filter` operators. This ensures that the solver only considers rows that satisfy the `WHERE` clause, significantly reducing the problem size.

### 3.3 Physical Execution
The `PhysicalDecide` operator works in a "stop-and-go" fashion (pipeline breaker):
1.  **Sink Phase**: It consumes all input tuples from its child operator (the candidate items). These tuples are buffered in memory.
2.  **Model Building**: It iterates over the buffered tuples to compute the coefficients for the objective function and constraints. A standard linear programming matrix (A-matrix) is constructed.
3.  **Solve Phase**: The constructed model is passed to `ILPModel::Build()` which creates a solver-agnostic representation, then `SolveILP()` dispatches to Gurobi (primary, significantly faster in practice) or HiGHS (slow fallback if Gurobi is unavailable).
4.  **Source Phase**: Once the solver returns, the operator augments the buffered tuples with the solution values (e.g., `x=1` or `x=0`) and streams them to the next operator (e.g., `SELECT` list projection).

> **Note**: The execution phase is documented in detail across five sub-documents: expression analysis (03a), coefficient evaluation (03b), model building (03c), solver backends (03d), and result projection (03e). See each for implementation details.

## 4. Integration Point
PackDB links against DuckDB as a loadable extension. It registers:
-   New Parser Keywords: `DECIDE`, `SUCH THAT`, `MAXIMIZE`, `MINIMIZE`.
-   New Transformer Rules: To convert parsed nodes into logical operators.
-   New Physical Operator: `PhysicalDecide`.
-   EXPLAIN Support: Both `LogicalDecide` and `PhysicalDecide` override `GetName()` and `ParamsToString()` to produce structured DECIDE node output in `EXPLAIN`, `EXPLAIN ANALYZE`, and `EXPLAIN (FORMAT JSON)`. See `01_pipeline/04_explain.md` for details.
