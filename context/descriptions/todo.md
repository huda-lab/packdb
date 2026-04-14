# PackDB — TODO

## Feasibility Problems (No Objective)

The grammar currently requires `MAXIMIZE` or `MINIMIZE` — omitting the objective clause produces a parse error. Both solver backends support feasibility natively; the only gap is the grammar.

Requires making the objective clause optional in `third_party/libpg_query/grammar/statements/select.y`. See `03_expressivity/problem_types/todo.md` for full details.

## NULL Coefficient Handling

NULL values in constraint/objective coefficients currently produce an error. Need to decide: silently treat as 0 (SQL semantics) or keep requiring explicit `COALESCE()`?

See `03_expressivity/such_that/todo.md` for the design tradeoffs.
