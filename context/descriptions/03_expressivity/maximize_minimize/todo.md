# MAXIMIZE / MINIMIZE — Planned Features

## Quadratic Constraints (QCQP) — Deferred

Quadratically Constrained Quadratic Programming (QCQP) would extend the current QP support to allow quadratic expressions in `SUCH THAT` clauses:

```sql
SUCH THAT SUM(POWER(new_val - old_val, 2)) <= 1000
```

This was explicitly deferred due to:
- Significantly increased architectural complexity (constraint matrices need Q matrices too)
- HiGHS does not support quadratic constraints at all
- Requires strict syntax enforcement for convexity: only `<=` with positive RHS, only sum-of-squares form
- Gurobi supports QCQP but with different API (`GRBaddqconstr`)

If implemented, the same syntax-enforced convexity approach should apply: only `SUM(POWER(linear_expr, 2)) <= positive_constant` should be accepted, guaranteeing the feasible region is convex (intersection of ellipsoids).
