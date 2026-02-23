# DECIDE Clause — Planned Features

## IS REAL Variables

**Priority: High** (blocks ABS linearization, imputation, repair, and synthesis tasks)

### What It Enables

Many real-world tasks require continuous-valued decision variables:
- **Data imputation**: `DECIDE imputed_distance IS REAL` — fill in missing numeric values
- **Data repair**: `DECIDE new_hours IS REAL` — adjust numeric attributes to satisfy constraints
- **Data synthesis**: `DECIDE syn_rent IS REAL` — generate synthetic numeric data matching aggregate statistics

Without IS REAL, PackDB can only solve selection problems (pick/don't pick) and counting problems (how many), not value-assignment problems (what value).

### Current State

The grammar already parses `IS REAL` (`select.y:170-177`, the `variable_type` rule includes `REAL`). However, the binder explicitly rejects it:

```cpp
// bind_select_node.cpp:470-474
if (type_marker == "real_variable") {
    throw BinderException(*expr_ptr,
        "DECIDE variable '%s' cannot be REAL type. ...");
}
```

### Suggested Implementation

1. **Remove the REAL rejection** in `bind_select_node.cpp:470-474`.
2. **Extend `SolverInput`** to carry a per-variable type flag (boolean / integer / continuous).
3. **Update solver backends**:
   - **HiGHS** (`deterministic_naive.cpp`): Use `HighsVarType::kContinuous` for REAL variables (HiGHS natively supports MILP with mixed variable types via `highs.changeColIntegrality()`).
   - **Gurobi** (`gurobi_solver.cpp`): Use `GRB_CONTINUOUS` instead of `GRB_INTEGER` for REAL variables (Gurobi natively supports this via `GRBaddvars()` vtype parameter).
4. **Default bounds**: REAL variables should default to `[0, +inf)` (same as INTEGER), unless the user adds explicit constraints.

### Dependencies

None — this is a self-contained change to the binder + solver interface.

### Impact

Unlocks imputation, repair, and synthesis use cases. Also a prerequisite for ABS linearization (see `sql_functions/todo.md`).
