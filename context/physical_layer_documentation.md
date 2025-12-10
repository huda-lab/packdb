# Implementation Part 3: Physical Execution & Optimization

## 1. Overview
The `PhysicalDecide` operator sits at the heart of PackDB's execution engine. It is a **blocking operator**, meaning it must consume its entire input before it can produce any output. This is necessary because the optimal value for any single decision variable depends on the entire dataset (global optimization).

**Key Source File**: `src/execution/operator/decide/physical_decide.cpp`

## 2. Execution Pipeline

### 2.1 Phase 1: Materialization (The Sink)
DuckDB executes the query plan up to the `PhysicalDecide` node. The operator acts as a "Sink", collecting every tuple that satisfies the `WHERE` clause.
-   **Storage**: Tuples are stored in a `ColumnDataCollection` (DuckDB's efficient in-memory columnar format).
-   **Memory Management**: If the dataset exceeds memory limits, DuckDB would normally spill to disk, but the current solver integration requires in-memory access to build the matrix.

### 2.2 Phase 2: Model Formulation
Once all data is collected, the operator transforms the data-centric view into a matrix-centric view for the solver.
1.  **Variable Instantiation**: For every row $r$ in the buffered data and every decision variable $x$, a corresponding solver column $Col_{r,x}$ is created.
2.  **Constraint Evaluation**:
    -   The operator evaluates the expression trees for the constraint coefficients.
    -   Example: If the constraint is `SUM(cost * x) <= 100`, the operator evaluates `cost` for every row to generate the vector coefficients $[c_1, c_2, ..., c_n]$.
3.  **Matrix Construction**: These values are passed to the HiGHS API to build the Constraint Matrix (Sparse format).

### 2.3 Phase 3: Solving (Highs Integration)
PackDB links directly to `libhighs`.
-   **API**: We use the Highs C++ API.
-   **Configuration**: The solver is configured with a time limit (default 60s) and a "Silent" logging profile to avoid polluting the database logs.
-   **Outcome**: The solver returns a status (Optimal, Infeasible, Unbounded) and a solution vector.

### 2.4 Phase 4: Result Projection (The Source)
If the solver finds an optimal solution:
1.  The operator iterates over the buffered rows again.
2.  It uses the map created in Phase 2 to look up the solution value for each row.
3.  These values are appended as new columns to the result chunk.
4.  The augmented chunk is passed up to the next operator (usually a `Projection` or `Limit`).

## 3. Data Consistency
A critical design choice is that PackDB guarantees **read consistency**. The optimization is performed on the snapshot of data seen by the query. Any concurrent modifications to the tables do not affect the running optimization model, as it works on the materialized buffer.
