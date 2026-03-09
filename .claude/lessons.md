# Lessons Learned

Corrections and patterns discovered during development. Updated after mistakes to prevent recurrence.

## Rules

- **Grammar files are templates**: Never edit `src_backend_parser_gram.cpp` directly. Edit `.y`/`.yh` files in `third_party/libpg_query/grammar/`, then run `python3 scripts/generate_grammar.py`.
- **Build before testing**: Always `make` (or `make debug`) after grammar or source changes before running tests.

## Gotchas

- **Don't rewrite aggregates to SUM at parse time if the return type matters**: Renaming `AVG(x)` → `SUM(x)` before binding changes the return type from DOUBLE to BIGINT (for integer inputs). DuckDB's comparison binding then casts fractional RHS values (e.g., `0.5`) to BIGINT → `0`, silently breaking constraints. Instead, let the original aggregate flow through binding natively (preserving its return type) and detect it at execution time via `BoundAggregateExpression::function.name`. This applies to any future aggregate rewrite where the original function's return type differs from SUM's.
- **Big-M disjunction sign convention**: For `x ≠ K` via Big-M, BOTH constraints must have z coefficient = `-M` (not `+M`/`-M`). Correct: `x - M*z ≤ K-1` (z=0 → x ≤ K-1) and `x - M*z ≥ K+1-M` (z=1 → x ≥ K+1). Wrong sign (+M on first constraint) makes z=0 trivially satisfy both constraints, leaving x unrestricted.
- **FindDecideVariable is "find any"**: `FindDecideVariable(expr)` recursively returns the first DECIDE variable found in the expression tree. For complex LHS expressions (e.g., `z_0 + z_1`), it finds only the first variable and silently ignores the rest. Use `ExtractLinearTerms` for multi-variable LHS, or check `GetExpressionClass() == BOUND_COLUMN_REF` first.
- **FunctionExpression::is_operator defaults false**: Programmatically-created `FunctionExpression("+", ...)` has `is_operator=false`, unlike parser-created ones where it's `true`. Don't gate logic on `is_operator` for expressions from rewrites.
<!-- Add entries here as they come up, format: short description + what to do instead -->
