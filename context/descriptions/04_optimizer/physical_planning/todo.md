# Physical Planning — Planned Features

---

## Cost-Based Solver/Strategy Selection

**Priority: Low** (useful for large-scale deployment, not immediate)

Currently solver selection is static (Gurobi > HiGHS). A cost-based approach would choose between execution strategies based on estimated problem characteristics:

| Strategy | Best For | Trade-off |
|---|---|---|
| Direct ILP (Gurobi) | Small-medium problems (< 10K rows) | Exact optimal, may be slow for large |
| Direct ILP (HiGHS) | When Gurobi unavailable | Slower but always available |
| LP relaxation + rounding | Large problems, near-optimality acceptable | Fast, approximate |
| Progressive Shading + LGS | Very large problems (100K+ rows) | Requires index, amortized over queries |

### Implementation Approach

1. Estimate problem size (row count, variable count, constraint count)
2. Estimate solver time using a simple model (e.g., linear in matrix non-zeros)
3. Choose strategy that meets a target time budget

This requires cost models for solver execution time, which could be:
- Simple heuristics based on matrix dimensions
- ML-based models trained on historical query execution data
