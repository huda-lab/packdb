# WHEN Keyword — Planned Features

## Known Feature Gaps

### `decide_when_condition` Grammar Is Restricted (`c_expr`)

The `decide_when_condition` non-terminal is `c_expr` — a restricted expression grammar that excludes:

- comparison operators (`=`, `<`, `>`, `<=`, `>=`, `<>`)
- logical `NOT`
- arithmetic (`+`, `-`)

Any of those tokens left unparenthesized inside a `WHEN` clause fails. Wrapping the condition in parentheses forces it through a different grammar production that supports the full set.

```sql
-- All of these FAIL to parse or misparse without parens:
SUM(x * v) WHEN tier = 'high' <= 10
SUM(x * v) <= 12 WHEN NOT w
SUM(x * v) <= 12 WHEN a + b > 5

-- All work when parenthesized:
SUM(x * v) WHEN (tier = 'high') <= 10
SUM(x * v) <= 12 WHEN (NOT w)
SUM(x * v) <= 12 WHEN (a + b > 5)
```

### Constraint vs. Objective Error Behavior Is Asymmetric

Unparenthesized WHEN conditions behave differently across constraint side, objective side, and condition shape. The error PHASE (parser vs. binder) and the resulting message text vary by combination. Empirically verified — pinned in `test/decide/tests/test_when_grammar.py`:

| WHEN shape | Constraint side | Objective side |
|---|---|---|
| `WHEN x = y` (comparison) | Parser error: `syntax error at or near "<="` (the `<=` token after the comparison is unparseable inside `c_expr`) | **Works** — `ReassociateObjectiveWhenComparison()` in `decide_symbolic.cpp` rewrites `(SUM(...) WHEN x) = y` into `SUM(...) WHEN (x = y)` |
| `WHEN NOT x` | Parser error: `syntax error at or near "NOT"` | Parser error: `syntax error at or near "NOT"` (reassociator only handles comparison-of-aggregate, not unary NOT) |
| `WHEN a + b > 5` | Parser error: `syntax error at or near "<="` | Binder error: `[MAXIMIZE\|MINIMIZE] clause does not support '...'(ExpressionClass::COMPARISON)` (parser succeeds via reassociator path but the resulting expression is not a valid objective component) |

```sql
-- All FAIL without parens:
SUM(x * value) WHEN tier = 'high' <= 10            -- constraint: parser
SUM(x * value) <= 12 WHEN NOT w                     -- both sides: parser
SUM(x * value) <= 12 WHEN a + b > 5                 -- constraint: parser
MAXIMIZE SUM(x * value) WHEN NOT w                  -- objective:  parser
MAXIMIZE SUM(x * value) WHEN a + b > 5              -- objective:  binder

-- All work when parenthesized:
SUM(x * value) WHEN (tier = 'high') <= 10
SUM(x * value) <= 12 WHEN (NOT w)
SUM(x * value) <= 12 WHEN (a + b > 5)
```

**Practical guidance**: always parenthesize non-trivial WHEN conditions. The objective-side reassociator works only on the simplest comparison-of-aggregate shape; everything else fails on either side.

**Potential fixes**: (a) extend `ReassociateObjectiveWhenComparison()` to cover constraints (would handle `WHEN x = y` on the constraint side); (b) widen the `decide_when_condition` grammar to admit `NOT`, comparisons, and arithmetic directly (requires `make grammar-build`) — this is the cleanest fix but touches the regenerated parser; (c) at minimum, improve the constraint-side parser error to hint that WHEN conditions need parentheses.
