# Physical Planning — Implemented Features

---

## Solver Selection (Gurobi / HiGHS Fallback)

PackDB uses a static solver dispatch policy: try Gurobi first, fall back to HiGHS if unavailable.

### How It Works

```
SolveILP(input):
    model = ILPModel::Build(input)
    if GurobiSolver::IsAvailable():
        return GurobiSolver::Solve(model)
    else:
        return DeterministicNaive::Solve(model)   // HiGHS
```

- **Gurobi**: Commercial solver, significantly faster on large ILP problems. Requires license (free for academic use). Uses the Gurobi C API.
- **HiGHS**: Open-source solver, bundled with PackDB. Slower but always available.

The selection is **not cost-based** — Gurobi is always preferred regardless of problem characteristics.

### Code Pointers

- **Dispatch logic**: `src/packdb/utility/ilp_solver.cpp:8-15`
- **Gurobi backend**: `src/packdb/gurobi/gurobi_solver.cpp` (full C API integration, ~275 lines)
  - `GurobiSolver::IsAvailable()` checks for Gurobi library at runtime
  - `GurobiSolver::Solve()` builds Gurobi model, adds variables/constraints/objective, calls `GRBoptimize()`
- **HiGHS backend**: `src/packdb/naive/deterministic_naive.cpp` (~189 lines)
  - Uses `Highs` C++ API
  - Builds `HighsLp` model with constraint matrix in CSC format
  - Calls `highs.run()` and extracts solution
- **Model builder**: `src/packdb/utility/ilp_solver.cpp` — `ILPModel::Build()` converts `SolverInput` into solver-agnostic matrix format
