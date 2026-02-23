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

## Layered Grouping Structure (LGS)

A data structure that groups tuples by constraint participation, enabling efficient incremental constraint evaluation.

### How It Helps PER

When `PER` generates O(|groups|) constraints, LGS pre-groups tuples so that each constraint's coefficient vector can be computed by lookup rather than full-relation scan.

### Relation to PER

LGS is specifically motivated by PER-generated constraints. Without PER, constraint counts are small (user-written) and this optimization is unnecessary. With PER on high-cardinality columns, LGS becomes essential.
