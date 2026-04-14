# WHEN Keyword — Planned Features

## Known Feature Gaps

### Aggregate-local WHEN + PER Composition

`SUM(x * value) WHEN priority <= 12 PER grp` is rejected by the PER binder because aggregate-local WHEN wraps the SUM in a `WHEN_CONSTRAINT_TAG` that the PER validation does not recognize as an aggregate constraint.

### Aggregate-local WHEN + MIN/MAX Rewrite

`MAX(x * value) WHEN eligible <= 7` produces incorrect results. The easy-case MAX-to-per-row rewrite applies `x * value <= 7` to all rows instead of only eligible rows. The optimizer's MIN/MAX rewrite does not carry the aggregate-local WHEN filter into the generated per-row constraints.

### Aggregate-local WHEN + Not-Equal (`<>`) Indicator

`SUM(x) WHEN active <> 2` produces incorrect results. The NE indicator rewrite (which creates an auxiliary Boolean variable to enforce disjunctive not-equal constraints) does not account for the aggregate-local WHEN filter. The indicator links against unfiltered row coefficients, so the `<>` constraint is effectively ignored or applied to the wrong subset of rows.

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
