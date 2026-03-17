# Rewrite Passes — Todo

New optimizer-level rewrites and migration of existing binder rewrites into the optimizer framework.

---

## 1. Constraint Push-Down

**Priority**: High

Push constraint evaluation closer to the data access layer to eliminate rows that cannot participate in any feasible solution.

**Approach**: Analyze constraint bounds to derive row-level implications. If a row's data makes it impossible for that row's decision variable to contribute to any feasible solution, prune it before building the ILP matrix.

**Example**: If `x IS BOOLEAN` and the only constraint involving x is `SUM(x * weight) <= 40`, then any row where `weight > 40` can never have `x = 1` in a feasible solution (a single such row would violate the constraint). These rows can be pruned.

**Requirements**:
- Bound analysis over constraint coefficients
- Must be conservative (only prune rows that are provably infeasible)
- Interacts with WHEN conditions (WHEN-excluded rows are already effectively pruned)

**Benefit**: Reduces number of decision variables and constraint matrix size. Most impactful for queries with tight constraints on large tables.

---

## 2. Constraint Pull-Out

**Priority**: Medium

Extract common sub-expressions from PER-generated constraints. When PER creates K copies of a constraint (one per group), many coefficient computations are shared across groups.

**Example**: `SUM(x * weight) <= 40 PER empID` with 1000 employees. The `weight` column values are the same regardless of which employee group we're computing — we only need to compute the coefficient vector once, then mask it K times.

**Current behavior**: Each group's constraint coefficients are computed independently during ILP model building. With pull-out, shared computation would be factored out.

**Requirements**:
- Identify which coefficient computations are group-independent
- Factor out shared work and apply group masks
- Most valuable when PER cardinality is high (many groups)

**Benefit**: Reduces ILP construction time (not solve time). Becomes critical for high-cardinality PER columns (1000+ distinct values).

---

## 3. Binder-to-Optimizer Migration

**Priority**: Medium

Move remaining binder-level rewrites into the `DecideOptimizer` pass for consistency, composability, and reduced core DuckDB footprint.

**Rewrites to migrate**:
- `RewriteCountToSum` (currently in `bind_select_node.cpp:414-467`)
- `RewriteAbsLinearization` (currently in `bind_select_node.cpp:527-546`, partially in optimizer already)
- `RewriteMinMaxInExpression` (currently in `bind_select_node.cpp:559+`)
- AVG rewrite (currently flag-based across binder + execution)

**Why migrate**:
- **Reduced DuckDB core modifications**: The binder is core DuckDB code; moving rewrites to the optimizer (PackDB-specific code) reduces upstream coupling
- **Better composability**: Optimizer passes can be reordered, enabled/disabled, and tested independently
- **Unified framework**: All DECIDE rewrites in one place (`DecideOptimizer`) rather than split across binder and optimizer

**Approach**: The `DecideOptimizer` already handles `RewriteNotEqual`, `RewriteCount`, and `RewriteAbs`. Extend it with additional passes for the remaining rewrites. Each rewrite becomes a method on `DecideOptimizer` operating on `LogicalDecide`.

**Note**: Some rewrites (like COUNT → SUM) create new variables, which currently happens during binding when the variable list is being built. Migration may require the optimizer to be able to add variables to `LogicalDecide` after binding, which needs careful design.
