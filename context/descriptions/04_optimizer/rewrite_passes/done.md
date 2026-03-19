# Rewrite Passes — Done

All DECIDE algebraic rewrites are now in the **DecideOptimizer** pass (logical-level transforms after binding). The binder validates and binds expressions; the optimizer rewrites them.

## Current Rewrite Locations

| Rewrite | Stage | Function | File |
|---------|-------|----------|------|
| ABS linearization (full) | Optimizer | `DecideOptimizer::RewriteAbs` | `decide_optimizer.cpp` |
| MIN/MAX linearization | Optimizer | `DecideOptimizer::RewriteMinMax` | `decide_optimizer.cpp` |
| `<>` disjunction | Optimizer | `DecideOptimizer::RewriteNotEqual` | `decide_optimizer.cpp` |
| COUNT → SUM (BOOLEAN/INTEGER) | Optimizer | `DecideOptimizer::RewriteCountToSum` | `decide_optimizer.cpp` |
| AVG → SUM (RHS scaling) | Optimizer | `DecideOptimizer::RewriteAvgToSum` | `decide_optimizer.cpp` |

The binder recognizes COUNT, AVG, ABS, MIN, and MAX as valid DECIDE aggregates/functions and binds them into their respective `BoundExpression` nodes. The optimizer then rewrites them:
- **ABS linearization**: Detects `BoundFunctionExpression` for ABS over decide variables. Creates auxiliary REAL variables, replaces ABS nodes with aux var references, and generates linearization constraints (`aux >= inner`, `aux >= -inner`).
- **MIN/MAX linearization**: Classifies MIN/MAX constraints as easy (strip aggregate → per-row) or hard (create BOOLEAN indicator, rewrite to SUM). Handles equality splitting, WHEN/PER wrappers, and objectives (flat and nested PER). Populates `minmax_indicator_links` on `LogicalDecide`. For objectives, sets typed metadata: `flat_objective_agg`/`flat_objective_is_easy` (flat) and `per_inner_agg`/`per_outer_agg`/`per_inner_is_easy`/`per_outer_is_easy` (PER). The easy/hard classification is pre-computed at optimization time so the physical layer reads the decision directly.
- **COUNT → SUM**: Creates indicator variables for INTEGER variables, populates `count_indicator_links`, and generates the `z <= x` linking constraint (forces z=0 when x=0). The companion constraint `x <= M*z` (forces z=1 when x>0) remains in physical execution because M depends on runtime variable bounds.
- **AVG → SUM**: Replaces AVG with SUM and tags with `AVG_REWRITE_TAG` for RHS scaling at execution time. Objectives get no tag (same argmax/argmin).
- **`<>` disjunction**: Creates BOOLEAN indicator variables for not-equal comparisons.

This separation keeps semantic validation in the binder and algebraic transformation in the optimizer.

**Cross-reference**: See `existing_optimizations/done.md` for detailed descriptions of each rewrite, including easy/hard case classification for MIN/MAX.
