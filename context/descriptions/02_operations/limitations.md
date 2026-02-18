# Limitations & Future Work

## 1. Current Limitations

While PackDB demonstrates the feasibility of in-database optimization, the current implementation has specific constraints:

### 1.1 Linearity Requirement
The system exclusively supports **Linear Programming (LP)** and **Integer Linear Programming (ILP)**.
-   Constraints must be linear: $\sum c_i x_i \le K$.
-   Objectives must be linear: $\max \sum c_i x_i$.
-   **Impact**: Users cannot natively express Quadratic Programming (QP) problems (e.g., Mean-Variance Portfolio Optimization) without linearization tricks.

### 1.2 Subquery Scope
Subqueries in the `SUCH THAT` clause are restricted to **uncorrelated scalar subqueries**.
-   **Supported**: `x <= (SELECT avg(budget) FROM Depts)` (Calculated once).
-   **Unsupported**: `x <= (SELECT budget FROM Depts WHERE Depts.id = item.dept_id)` (Correlated).
-   **Reason**: Correlated subqueries would require interacting with the solver logic per-row in a way that currently breaks the matrix formulation pipeline.

### 1.3 Scalability (Exact Solver)
Both Gurobi and HiGHS are exact solvers. For NP-Hard problems (integer optimization), the run-time can grow exponentially with the number of decision variables (rows).
-   **Current Limit**: Effective for thousands of rows (HiGHS) to tens of thousands (Gurobi).
-   **Bottleneck**: For millions of rows, an exact IP solver will time out.

## 2. Future Work

### 2.1 Approximate Solvers (Heuristics)
To handle "Big Data" optimization (millions of rows), future versions could integrate heuristic solvers (e.g., Greedy approaches, Simulated Annealing) or relaxation-based approaches (solving the LP relaxation and rounding).
-   **Benefit**: Near-optimal solutions in linear time.
-   **Trade-off**: No guarantee of global optimality.

### 2.2 Correlated Subquery Unnesting
Implementing a binder rule to "unnest" correlated subqueries into joins before the `DECIDE` clause would allow more complex constraints to be supported natively.

### 2.3 Incremental Optimization
Currently, the entire dataset is materialized. Future work could explore "Interactive Optimization," where the user provides feedback on a solution, and the solver incrementally updates the result without re-building the entire model.

### 2.4 Conditional Expressions (`WHEN` Keyword) — Implemented
The `WHEN` postfix keyword is now supported for both constraints (`SUCH THAT`) and objectives (`MAXIMIZE`/`MINIMIZE`).
-   **Constraint syntax**: `SUM(x * weight) <= 20 WHEN category = 'perishable'`
-   **Objective syntax**: `MAXIMIZE SUM(x * profit) WHEN category = 'electronics'`
-   **Benefit**: More expressive modeling without query decomposition. Objective WHEN zeros out coefficients for non-matching rows.
-   **Limitation**: WHEN conditions cannot reference decision variables.

### 2.5 Gurobi Solver Integration — Implemented
PackDB now supports Gurobi as the primary solver with automatic fallback to HiGHS.
-   **Mechanism**: At solve time, PackDB attempts to use Gurobi first. If Gurobi is not available (no license installed), it falls back to the bundled HiGHS solver.
-   **Benefit**: Gurobi offers significantly better performance on large-scale ILP problems.
-   **Requirement**: Gurobi requires a commercial license (free for academic use).
