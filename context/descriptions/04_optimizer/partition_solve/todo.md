# Partition-Solve — Todo

## PER Decomposition

**Priority**: High

When all constraints AND objective share the same PER column with no global constraints, the ILP decomposes into K independent sub-problems (one per distinct PER value). Solving K problems with N/K variables each is dramatically faster than solving one problem with N variables.

**Example**: 10,000 rows, 100 employees, all constraints `PER empID` → 100 independent ILPs of 100 variables each (milliseconds) instead of a single ILP with 10,000 variables (seconds/minutes).

### Detection Criteria

1. Every constraint has `per_column` set (no global constraints)
2. All constraints use the **same** PER column
3. Objective either uses the same PER column or is absent
4. No coefficient references columns from other groups

With the unified `row_group_ids` design, detection is simple: all constraints' `row_group_ids` must be non-empty and identical; `num_groups` tells us K.

### Implementation

Check in `physical_decide.cpp` Finalize:

```cpp
// Detection
Check all LinearConstraints have per_column
All per_columns reference same column binding
Objective per_column matches or absent

// If yes → partition-solve path:
Build group_rows index  // already computed by PER
For each group:
    Build sub-SolverInput (subset of rows in this group)
    Remap variable indices to local [0, group_size)
    Solve independently
    Map solution back to global variable indices
Combine all group solutions into global ilp_solution
```

### Fallback

If detection fails (mixed PER columns, global constraints present), fall back to the current monolithic solve path. No behavior change for non-PER queries.

### Parallelization

Each group is fully independent → can solve in parallel using DuckDB's task scheduler. With K groups and P threads, achieve up to min(K, P) speedup.

### Interaction with PER Objective

When partition-solve is implemented, `MINIMIZE SUM(...) PER col` becomes meaningful — minimize each group independently. Currently the nested aggregate PER objective syntax handles this at the ILP formulation level, but partition-solve would make it a true per-group optimization.

### Scaling Impact

| Scenario | Without partition-solve | With partition-solve |
|----------|----------------------|---------------------|
| 10K rows, 100 groups | 1 ILP, 10K vars | 100 ILPs, 100 vars each |
| 100K rows, 1000 groups | 1 ILP, 100K vars (may timeout) | 1000 ILPs, 100 vars each |
| Mixed global + PER | 1 ILP (no change) | 1 ILP (fallback) |
