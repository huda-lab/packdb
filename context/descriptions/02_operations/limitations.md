# Limitations & Future Work

## 1. Current Limitations

While PackDB demonstrates the feasibility of in-database optimization, the current implementation has specific constraints:

### 1.1 Linearity Requirement
The system exclusively supports **Linear Programming (LP)** and **Integer Linear Programming (ILP)**.
-   Constraints must be linear: $\sum c_i x_i \le K$.
-   Objectives must be linear: $\max \sum c_i x_i$.
-   **Impact**: Users cannot natively express Quadratic Programming (QP) problems (e.g., Mean-Variance Portfolio Optimization) without linearization tricks.

### 1.2 Scalability (Exact Solver)
Both Gurobi and HiGHS are exact solvers. For NP-Hard problems (integer optimization), the run-time can grow exponentially with the number of decision variables (rows).
-   **Benchmark data** (HiGHS, commit 6b56b35): ~500 rows solves in ~60ms, ~2000 rows in ~290ms, ~10000 rows in ~1450ms (solver time only, simple binary knapsack).
-   **Current Limit**: Effective for thousands of rows (HiGHS) to tens of thousands (Gurobi).
-   **Bottleneck**: For millions of rows, an exact IP solver will time out. Solver time dominates at ~95% of total execution time at scale (see `02_operations/benchmarking.md`).

## 2. Future Work

### 2.1 Approximate Solvers (Heuristics)
To handle "Big Data" optimization (millions of rows), future versions could integrate heuristic solvers (e.g., Greedy approaches, Simulated Annealing) or relaxation-based approaches (solving the LP relaxation and rounding).
-   **Benefit**: Near-optimal solutions in linear time.
-   **Trade-off**: No guarantee of global optimality.

### 2.2 Incremental Optimization
Currently, the entire dataset is materialized. Future work could explore "Interactive Optimization," where the user provides feedback on a solution, and the solver incrementally updates the result without re-building the entire model.

## 3. Completed (Previously Future Work)

### ~~3.1 Conditional Expressions (`WHEN` Keyword)~~
Implemented. See `context/descriptions/03_expressivity/when/done.md`.

### ~~3.2 Gurobi Solver Integration~~
Implemented. See `context/descriptions/04_optimizer/existing_optimizations/done.md` §3.

### ~~3.3 Correlated Subquery Unnesting~~
Implemented. Correlated subqueries in `SUCH THAT` constraints are unnested into joins before the DECIDE clause. See `test/decide/tests/test_cons_correlated_subquery.py`.
