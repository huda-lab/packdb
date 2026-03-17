# Rewrite Passes — Done

Current algebraic rewrites are split between the **binder** (expression-level transforms during `bind_select_node.cpp`) and the **DecideOptimizer** pass (logical-level transforms after binding).

## Current Rewrite Locations

| Rewrite | Stage | Function | File |
|---------|-------|----------|------|
| COUNT → SUM (BOOLEAN) | Binder | `RewriteCountToSum` | `bind_select_node.cpp:414-467` |
| COUNT → SUM (INTEGER, Big-M) | Binder | `RewriteCountToSum` | `bind_select_node.cpp:414-467` |
| AVG → SUM (RHS scaling) | Binder + Execution | flag at `physical_decide.cpp:391` | scaling at `ilp_model_builder.cpp:164-166, 211-213` |
| ABS → auxiliary var | Binder | `RewriteAbsLinearization` | `bind_select_node.cpp:527-546` |
| ABS linearization constraints | Optimizer | `DecideOptimizer::RewriteAbs` | `decide_optimizer.cpp:105-136` |
| MIN/MAX linearization | Binder | `RewriteMinMaxInExpression` | `bind_select_node.cpp:559+` |
| `<>` disjunction | Optimizer | `DecideOptimizer::RewriteNotEqual` | `decide_optimizer.cpp:38-47` |
| COUNT indicator linking | Optimizer | `DecideOptimizer::RewriteCount` | `decide_optimizer.cpp:84-103` |

The binder rewrites were the first transforms implemented and operate on `ParsedExpression` trees. The optimizer rewrites were added later and operate on `LogicalDecide` nodes within DuckDB's optimizer framework.

**Cross-reference**: See `existing_optimizations/done.md` for detailed descriptions of each rewrite, including easy/hard case classification for MIN/MAX.
