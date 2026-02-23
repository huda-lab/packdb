# Adaptive Processing — Planned Features

---

## Solver Time Limit

**Priority: High** (simple to implement, prevents runaway queries)

Neither solver backend currently sets a timeout. Both Gurobi and HiGHS support time limits natively:

- **Gurobi**: `GRBsetdblparam(env, GRB_DBL_PAR_TIMELIMIT, seconds)` — set before `GRBoptimize()`
- **HiGHS**: `highs.setOptionValue("time_limit", seconds)` — set before `highs.run()`

### Suggested Implementation

1. Add a configurable timeout parameter (e.g., `SET decide_timeout = 60`)
2. Pass the timeout through `SolverInput` to both solver backends
3. Default: 60 seconds (reasonable for interactive queries)
4. When timeout is reached, return the best feasible solution found so far (both solvers support this)

---

## Constraint Softening (Slack Variables)

**Priority: Medium**

Automatically add slack variables to each constraint to avoid infeasible results for over-constrained problems.

### Idea

Replace `SUM(x * w) <= K` with `SUM(x * w) <= K + epsilon`, where `epsilon` is a non-negative slack variable. Minimize total slack in the objective (or use binary search over epsilon tolerance).

### Benefit

Instead of "INFEASIBLE" errors, return the best approximate solution with minimal constraint violation. Enables interactive "anytime" solving.

---

## Objective Hardening (Sparsity Promotion)

**Priority: Low**

For repair/explanation tasks where "minimize total deviation" admits many diffuse solutions, reformulate to "minimize number of edits" (L0-style) to favor sparse, interpretable changes.

### Approach

Replace continuous deviations with binary "did this row change?" indicators. This converts a continuous objective into a binary selection problem that the solver handles more efficiently.

---

## Incremental Reasoning (Solution Maintenance)

**Priority: Low** (depends on skyband indexing)

When data or constraints change, determine whether:
1. The last solution still holds (no re-solve needed)
2. The solution can be incrementally updated
3. A full re-solve is warranted

Uses the skyband index and LGS (see [../problem_reduction/todo.md](../problem_reduction/todo.md)) to make this determination efficiently.
