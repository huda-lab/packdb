# Lessons Learned

Corrections and patterns discovered during development. Update this after mistakes to prevent recurrence.

## Rules

- Grammar files are templates. Never edit `src_backend_parser_gram.cpp` directly. Edit `.y` or `.yh` files in `third_party/libpg_query/grammar/`, then run `python3 scripts/generate_grammar.py` or the repo grammar make target.
- Build before testing. Always run `make` or `make debug` after grammar or source changes before running tests.

## Gotchas

- Do not rewrite aggregates to `SUM` at parse time if the return type matters. Renaming `AVG(x)` to `SUM(x)` before binding changes the return type from `DOUBLE` to `BIGINT` for integer inputs. DuckDB's comparison binding then casts fractional RHS values such as `0.5` to `BIGINT`, silently breaking constraints. Let the original aggregate flow through binding and detect it later via `BoundAggregateExpression::function.name`.
- Big-M not-equal sign convention: for `x <> K`, both constraints must have z coefficient `-M`. Correct: `x - M*z <= K-1` for the low branch and `x - M*z >= K+1-M` for the high branch. A positive `M` on the first constraint can leave `x` unrestricted.
- `FindDecideVariable(expr)` returns the first DECIDE variable found in the expression tree. For complex LHS expressions such as `z_0 + z_1`, use `ExtractLinearTerms` or check `GetExpressionClass() == BOUND_COLUMN_REF` first.
- Programmatically-created `FunctionExpression("+", ...)` has `is_operator = false`, unlike parser-created operator expressions. Do not gate rewrite logic on `is_operator` for expressions created by rewrites.

