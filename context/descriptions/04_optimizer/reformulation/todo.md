# Reformulation — Planned Features

**Priority: Lower than query rewriting — focus on Big-M and push-down first.**

---

## LP Relaxation + Rounding

Solve the continuous relaxation (drop integrality constraints) first, then round the fractional solution to integers. This provides:
1. A fast lower/upper bound on the optimal value
2. An approximate feasible solution (after rounding)

### When to Use

- Large problems where exact ILP is too slow
- Problems where the LP relaxation is known to be tight (e.g., totally unimodular constraint matrices)
- As a component of a branch-and-bound scheme

### Implementation Approach

Both HiGHS and Gurobi support solving LP relaxations natively. The change is to pass all variables as continuous (regardless of declared type) and add a rounding post-processing step.

---

## Constraint Tightening (Chvátal-Gomory Cuts)

Strengthen constraints to reduce the feasible region of the LP relaxation, improving solver performance on the ILP.

### Idea

Given integer constraints, derive new valid inequalities that cut off fractional LP solutions. For example, if `2x + 3y <= 7` and x,y are integer, then `x + y <= 2` is a valid tightening.

### Note

Most modern ILP solvers (Gurobi, HiGHS) already generate cutting planes internally. This optimization would only be valuable if we can exploit problem structure visible at the query level but not at the solver level.

---

## Symmetry Breaking

Add constraints that eliminate equivalent solutions when decision variables are interchangeable.

### Example

If selecting 5 items from a pool of identical items, many permutations give the same objective value. Adding ordering constraints (`x_1 >= x_2 >= ... >= x_n`) eliminates symmetric branches in the search tree.

### Detection

Identify groups of rows with identical constraint coefficients and objective contributions. These form symmetry groups where the solver wastes time exploring equivalent branches.
