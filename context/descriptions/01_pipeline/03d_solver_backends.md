# Phase 4: Solver Backends

## Overview

PackDB uses a solver facade pattern. The `SolveILP()` function in `ilp_solver.cpp` builds an `ILPModel` from the `SolverInput`, then dispatches to the best available solver backend. The function is 6 lines of logic:

```cpp
vector<double> SolveILP(const SolverInput &input) {
    ILPModel model = ILPModel::Build(input);
    if (GurobiSolver::IsAvailable()) {
        return GurobiSolver::Solve(model);
    }
    return DeterministicNaive::Solve(model);
}
```

**Key Source Files**:
- `src/packdb/utility/ilp_solver.cpp` (~17 lines)
- `src/packdb/gurobi/gurobi_solver.cpp` (~199 lines)
- `src/packdb/naive/deterministic_naive.cpp` (~189 lines)

## Solver Selection

- If `GurobiSolver::IsAvailable()` returns true, Gurobi is used. **Gurobi is the primary solver** and is strongly recommended — empirical benchmarking has shown it to be significantly faster than HiGHS for PackDB workloads.
- Otherwise, HiGHS (via `DeterministicNaive`) is used as a fallback. HiGHS is substantially slower in practice and should only be used when a Gurobi license is unavailable.

`GurobiSolver::IsAvailable()` performs a one-time lazy check (static local variable with lambda initialization): it attempts to create a Gurobi environment via `GRBloadenv()`. If compilation did not include Gurobi (`PACKDB_HAS_GUROBI` not defined), it always returns false.

## Gurobi Backend

Uses Gurobi's **C API** (`gurobi_c.h`), not the C++ wrapper.

### Resource Management

An RAII wrapper `GurobiGuard` manages `GRBmodel*` and `GRBenv*` lifetime. The destructor calls `GRBfreemodel()` and `GRBfreeenv()`, ensuring cleanup on all exit paths including exceptions.

### Model Setup

1. **Environment**: Created via `GRBloadenv()`. `OutputFlag` is set to 0 (silent output).
2. **Variables**: All variables are added in one `GRBnewmodel()` call, which takes the variable types, bounds, and objective coefficients as arrays:
   - `GRB_BINARY` for `is_binary` variables
   - `GRB_INTEGER` for `is_integer` (non-binary) variables
   - `GRB_CONTINUOUS` for continuous variables
3. **Model sense**: Set via `GRBsetintattr(GRB_INT_ATTR_MODELSENSE, ...)` to `GRB_MAXIMIZE` or `GRB_MINIMIZE`.
4. **Constraints**: Added individually via `GRBaddconstr()`, which directly accepts COO format (index array, coefficient array, sense char, RHS).

### Solving and Status

After `GRBoptimize()`, the status is checked:

| Gurobi Status | Action |
|---|---|
| `GRB_OPTIMAL` | Extract and return solution |
| `GRB_INFEASIBLE` | `InvalidInputException` with constraint diagnosis suggestions |
| `GRB_UNBOUNDED` / `GRB_INF_OR_UNBD` | `InvalidInputException` suggesting bounds |
| `GRB_TIME_LIMIT` | `InvalidInputException` about complexity |
| `GRB_ITERATION_LIMIT` | `InvalidInputException` about complexity |
| Other | Generic `InvalidInputException` with status code |

### Solution Extraction

Solution values are extracted via `GRBgetdblattrarray(GRB_DBL_ATTR_X, ...)` into a `vector<double>`. Each value is validated as finite (not NaN or Infinity).

## HiGHS Backend

> **Note**: HiGHS is retained as an open-source fallback for environments without a Gurobi license. Empirical benchmarking has shown it to be significantly slower than Gurobi for PackDB workloads. It is not recommended for production use.

Uses HiGHS's **C++ API** (`Highs.h`). Despite the class name `DeterministicNaive`, this is a full-featured MIP solver.

### Variable Types

HiGHS uses `HighsVarType::kInteger` or `HighsVarType::kContinuous`. There is no separate binary type -- binary variables are represented as integer variables with bounds [0, 1].

### Constraint Conversion

ILP sense characters are converted to HiGHS range format (`row_lower`, `row_upper`):

| Sense | `row_lower` | `row_upper` |
|---|---|---|
| `'>'` (>=) | `rhs` | `1e30` |
| `'<'` (<=) | `-1e30` | `rhs` |
| `'='` | `rhs` | `rhs` |

### Matrix Format Conversion

The `ILPModel` stores constraints in COO format (row index, column index, value triples). HiGHS requires CSR (Compressed Sparse Row) format with `start_`, `index_`, `value_` arrays.

The conversion:
1. Count non-zeros per row (via `row_starts[a_rows[i] + 1]++`)
2. Prefix sum to get row start offsets
3. Scatter COO entries into CSR positions using a `current_pos` tracking array

The matrix format is set to `MatrixFormat::kRowwise`.

### Model and Solving

The `HighsLp` struct is populated with:
- `col_cost_` (objective), `col_lower_`/`col_upper_` (variable bounds)
- `row_lower_`/`row_upper_` (constraint bounds)
- `a_matrix_` (constraint matrix in CSR)
- `integrality_` (variable types)
- `sense_` (`ObjSense::kMaximize` or `kMinimize`)

Logging is disabled with `log_to_console = false`. The model is passed via `highs.passModel(lp)` and solved with `highs.run()`.

### Status Handling

| HiGHS Status | Action |
|---|---|
| `kOptimal` | Extract and return solution |
| `kInfeasible` | `InvalidInputException` (same message as Gurobi) |
| `kUnbounded` | `InvalidInputException` (same message as Gurobi) |
| `kTimeLimit` | `InvalidInputException` |
| `kIterationLimit` | `InvalidInputException` |
| Other | Generic `InvalidInputException` with status code |

Solution is extracted from `highs.getSolution().col_value`, validated for completeness and finite values.

## Error Messages

Both backends produce identical user-facing error messages for each failure mode, ensuring a consistent experience regardless of which solver is active. Messages include:

- **Infeasible**: Lists common causes (contradictory bounds, impossible SUM constraints, overly restrictive variable types) and suggests relaxing constraints.
- **Unbounded**: Explains the objective can grow infinitely and suggests adding upper bounds, budget limits, or using BOOLEAN.
- **Time/Iteration Limit**: Suggests simplifying constraints or reducing data size.

## Adding a New Solver Backend

To add a new solver:

1. Create a new class with a static `Solve(const ILPModel &) -> vector<double>` method.
2. Optionally add a static `IsAvailable()` method for runtime detection.
3. Add a dispatch entry in `ilp_solver.cpp` (between Gurobi and HiGHS, or as a new priority level).
4. The `ILPModel` struct provides everything needed: variable bounds/types, objective coefficients, and constraints in COO format.

No changes to the expression analysis, coefficient evaluation, or model building phases are required.
