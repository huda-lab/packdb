# WHEN Keyword — Planned Features

## Known Feature Gaps

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
