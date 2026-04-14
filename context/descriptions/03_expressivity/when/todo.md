# WHEN Keyword — Planned Features

## Known Feature Gaps

### WHEN + Not-Equal (`<>`) Indicator (pre-existing, affects both expression-level and aggregate-local)

Both `SUM(x) <> 2 WHEN active` (expression-level) and `SUM(x) WHEN active <> 2` (aggregate-local) produce incorrect results. The NE indicator Big-M expansion does not correctly interact with WHEN-filtered row coefficients. The root cause is in the NE expansion section of `physical_decide.cpp` — the Big-M disjunction does not properly account for the reduced row set when computing indicator constraints.

### Aggregate-local WHEN Grammar Asymmetry

The `decide_when_condition` non-terminal is `c_expr` — a restricted expression that excludes comparison operators (`=`, `<`, `>`, `<=`, `>=`, `<>`) and arithmetic (`+`, `-`). Unparenthesized comparison conditions in aggregate-local WHEN are misinterpreted:

**In constraints**, the parser reassociates the comparison into a double comparison that the binder rejects:

```sql
-- User writes:
SUM(x * value) WHEN tier = 'high' <= 10

-- Parser produces:
((SUM(x * value) WHEN tier) = 'high') <= 10

-- Binder rejects the double comparison.
-- Fix: parenthesize the condition.
SUM(x * value) WHEN (tier = 'high') <= 10
```

**In objectives**, `ReassociateObjectiveWhenComparison()` in `decide_symbolic.cpp` detects when a comparison wraps an aggregate-local WHEN and transforms it back into expression-level WHEN:

```sql
-- User writes:
MAXIMIZE SUM(x * value) WHEN category = 'high'

-- Parser produces:
(SUM(x * value) WHEN category) = 'high'

-- Reassociator transforms to expression-level WHEN:
SUM(x * value) WHEN (category = 'high')
```

This preserves backward compatibility with legacy objective syntax. Users should always parenthesize comparison conditions in aggregate-local WHEN to avoid the asymmetry between constraints and objectives.
