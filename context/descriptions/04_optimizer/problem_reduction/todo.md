# Problem Reduction — Planned Features

**Priority: Medium — becomes critical for large datasets (100K+ rows).**

---

## Skyband Indexing

Precompute the set of "potentially optimal" tuples — those not dominated on all constraint/objective dimensions. Only these need solver attention.

### Core Idea

A tuple `t` is **dominated** by tuple `t'` if `t'` is at least as good on every objective dimension and at least as good (or tighter) on every constraint dimension. Dominated tuples can never appear in an optimal solution.

### Example

In a knapsack problem (maximize value, weight <= 50):
- Item A: value=10, weight=5
- Item B: value=8, weight=7

Item A dominates Item B (better value, lower weight). If the solution includes B, replacing it with A is always at least as good.

### Benefit

For many practical datasets, the skyband (set of non-dominated tuples) is much smaller than the full dataset. Preliminary estimates: 12ms incremental vs 300ms full solve on million-tuple datasets.

### Implementation

1. Build a multi-dimensional index over (objective coefficients, constraint coefficients) per decision variable
2. Compute the skyband using standard skyline algorithms
3. Pass only skyband tuples to the solver
4. Verify solution feasibility against the full dataset (in case of edge cases)

---

## Progressive Shading

Layer-by-layer evaluation using the skyband index. Process the most promising tuples first, prove optimality without processing all layers.

### Core Idea

1. Partition the skyband into layers (Layer 0 = skyline, Layer 1 = skyline of remaining tuples, etc.)
2. Solve using only Layer 0 tuples
3. Check if the solution can be improved by adding Layer 1 tuples
4. Continue until provably optimal or time limit reached

### Benefit

For many problems, Layer 0 alone contains the optimal solution. This avoids materializing the full dataset.

---

## Partition-Solve (PER Decomposition)

**Priority: High** (major performance win for PER-heavy queries)

When all constraints and the objective share the same PER column and there are no global (non-PER) constraints, the problem decomposes into K fully independent ILPs — one per PER group. Each can be solved separately.

### Why This Matters

ILP solving is worst-case exponential in the number of variables. Solving K problems with N/K variables each is dramatically faster than solving 1 problem with N variables. Even with polynomial-time LP relaxations, smaller problems have better constants and tighter bounds.

**Example**: 10,000 rows, 100 employees, all constraints PER empID:
- Single ILP: 10,000 binary variables → solver may take seconds to minutes
- Partition-solve: 100 ILPs of 100 variables each → each solves in milliseconds

### Detection Criteria

A DECIDE query is partition-decomposable when:
1. Every constraint has `per_column` set (no global constraints like `SUM(x) BETWEEN 200 AND 400`)
2. All constraints use the **same** PER column (or same set of PER columns when multi-column is supported)
3. The objective either has the same PER column or is absent
4. No constraint's coefficient expression references columns from other groups (this is guaranteed by the current single-table PER design)

### Implementation Approach

Detect partition-decomposability at the start of `Finalize` in `physical_decide.cpp`:

```
1. Check all LinearConstraints: do they all have per_column set?
2. Check all per_columns reference the same column binding
3. Check objective per_column matches (or objective has no PER)
4. If all checks pass → partition-solve path
```

**Partition-solve path**:
1. Evaluate the shared PER column for all rows → group assignment
2. Build `group_rows` index: which rows belong to each group
3. For each group:
   a. Build a sub-SolverInput with only that group's rows
   b. Remap variable indices to 0..group_size-1
   c. Call `SolveILP(sub_input)` independently
   d. Map solution back to global row indices
4. Combine all group solutions into the global `ilp_solution` vector

**Parallelization opportunity**: Each group's ILP is independent, so groups can be solved in parallel using DuckDB's task scheduler. This is a natural extension.

### Interaction with Existing Architecture

The unified `row_group_ids` design (see `solver_input.hpp`) makes detection easy:
- All constraints' `row_group_ids` must be non-empty and have the same group assignment
- The `num_groups` field immediately tells us K

The detection logic can be a simple check before calling `SolveILP`:

```cpp
bool all_same_per = true;
for (auto &ec : solver_input.constraints) {
    if (ec.row_group_ids.empty()) { all_same_per = false; break; }
    if (ec.row_group_ids != solver_input.constraints[0].row_group_ids) {
        all_same_per = false; break;
    }
}
if (all_same_per && no_global_constraints) {
    return PartitionSolve(solver_input);
}
```

### Fallback

When partition-solve criteria are not met (mixed PER columns, or some global constraints), fall back to the standard single-ILP path. The group-aware `ILPModel::Build` handles this correctly.

### Relation to PER on Objective

PER on the objective is currently a no-op (treated as global SUM). When partition-solve is implemented, `MINIMIZE SUM(...) PER col` becomes meaningful: it means "minimize each group's sum independently." This is automatically achieved by partition-solve since each sub-ILP has its own objective.

---

## Layered Grouping Structure (LGS)

A data structure that groups tuples by constraint participation, enabling efficient incremental constraint evaluation.

### How It Helps PER

When `PER` generates O(|groups|) constraints, LGS pre-groups tuples so that each constraint's coefficient vector can be computed by lookup rather than full-relation scan.

### Relation to PER

LGS is specifically motivated by PER-generated constraints. Without PER, constraint counts are small (user-written) and this optimization is unnecessary. With PER on high-cardinality columns, LGS becomes essential.
