# PackDB — TODO

## NULL Coefficient Handling

NULL values in constraint/objective coefficients currently produce an error. Need to decide: silently treat as 0 (SQL semantics) or keep requiring explicit `COALESCE()`?

See `03_expressivity/such_that/todo.md` for the design tradeoffs.
