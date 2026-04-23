# MIN/MAX Aggregate Test Coverage — Done

Tests live in:
- `test/decide/tests/test_min_max.py` — primary MIN/MAX test file
- `test/decide/tests/test_per_objective.py` — nested aggregate PER objectives
- `test/decide/tests/test_aggregate_local_when.py` — aggregate-local WHEN with MAX
- `test/decide/tests/test_per_interactions.py` — hard MIN/MAX constraints with PER (per-group Big-M)

MIN/MAX linearization is performed by `DecideOptimizer::RewriteMinMax`. It
classifies each occurrence as **easy** (naturally per-row, no Big-M) or
**hard** (needs a global auxiliary variable and per-row binary indicators).

## Scenarios covered

### Easy cases (no Big-M)

| Scenario | Where | Oracle |
|----------|-------|--------|
| `MAX(expr) <= K` → per-row | `test_min_max.py::test_max_leq_constraint` | ✓ |
| `MAX(x * expr) <= K` (column coefficient) | `test_min_max.py::test_max_leq_with_expr` | ✓ |
| `MAX(x) <= K` on INTEGER (per-row cap) | `test_min_max.py::test_max_leq_integer` | ✓ |
| `MIN(expr) >= K` → per-row | `test_min_max.py::test_min_geq_constraint` | ✓ |
| `MAX + WHEN` (easy) | `test_min_max.py::test_max_constraint_with_when` | ✓ |
| `MAX + PER` (stripped as redundant) | `test_min_max.py::test_max_constraint_with_per` | ✓ |
| `MAX(x) <= 0 WHEN cond PER col` (WHEN + PER composition) | `test_min_max.py::test_min_max_when_per_composition` | ✓ |
| `MAX <= K` with entity-scoped | `test_entity_scope.py::test_entity_scoped_with_max` | ✓ |
| `MIN >= K` with entity-scoped | `test_entity_scope.py::test_entity_scoped_min_easy_case` | ✓ |
| Aggregate-local WHEN on MAX (easy) | `test_aggregate_local_when.py::test_aggregate_local_when_with_max` | ✓ |

### Hard cases (Big-M indicators)

| Scenario | Where | Oracle |
|----------|-------|--------|
| `MAX(expr) >= K` | `test_min_max.py::test_max_geq_constraint` | ✓ |
| `MIN(expr) <= K` | `test_min_max.py::test_min_leq_constraint` | ✓ |
| `MAX(expr) = K` (equality) | `test_min_max.py::test_max_eq_constraint` | ✓ |
| `MIN(expr) = K` (equality) | `test_min_max.py::test_min_eq_constraint` | ✓ |
| Multiple MIN/MAX constraints in one query | `test_min_max.py::test_multiple_minmax_constraints` | ✓ |
| MIN/MAX in both constraint and objective | `test_min_max.py::test_minmax_constraint_and_objective` | ✓ |
| `MAX >= K` with entity-scoped | `test_entity_scope.py::test_entity_scoped_max_hard_case` | ✓ |
| `MAX(expr) >= K PER col` (Big-M per group) | `test_per_interactions.py::test_per_max_geq_constraint` | ✓ |
| `MIN(expr) <= K PER col` (Big-M per group) | `test_per_interactions.py::test_per_min_leq_constraint` | ✓ |
| `MAX(expr) = K PER col` (easy + hard combined per group) | `test_per_interactions.py::test_per_max_eq_constraint` | ✓ |
| `MAX(expr) WHEN cond >= K` (hard + aggregate-local WHEN) | `test_aggregate_local_when.py::test_aggregate_local_when_with_hard_max` | ✓ |

### Objectives

| Scenario | Where | Oracle |
|----------|-------|--------|
| `MINIMIZE MAX(expr)` (easy) | `test_min_max.py::test_minimize_max_objective` | ✓ |
| `MAXIMIZE MIN(expr)` (easy) | `test_min_max.py::test_maximize_min_objective` | ✓ |
| `MAXIMIZE MIN(expr) WHEN cond` (easy flat with aggregate-local WHEN) | `test_min_max.py::test_min_objective_with_when` | ✓ |
| `MINIMIZE MAX(expr) WHEN cond` (easy flat with aggregate-local WHEN) | `test_min_max.py::test_max_objective_with_when` | ✓ |
| `MINIMIZE MAX(x)` on INTEGER | `test_min_max.py::test_minimize_max_integer` | ✓ |
| `MAXIMIZE MAX(expr)` (hard) | `test_min_max.py::test_maximize_max_objective` | ✓ |
| `MINIMIZE MIN(expr)` (hard) | `test_min_max.py::test_minimize_min_objective` | ✓ |

### Nested aggregate + PER objectives

Tests in `test_per_objective.py` cover all 4 × 4 nesting combinations of inner/outer ∈ {SUM, MIN, MAX, AVG} with PER:

| Scenario | Test |
|----------|------|
| `MINIMIZE SUM(MAX(...)) PER col` | `test_minimize_sum_max_per` |
| `MAXIMIZE SUM(MIN(...)) PER col` | `test_maximize_sum_min_per` |
| `MAXIMIZE SUM(MAX(...)) PER col` | `test_maximize_sum_max_per` |
| `MINIMIZE SUM(MIN(...)) PER col` | `test_minimize_sum_min_per` |
| `MINIMIZE MAX(SUM(...)) PER col` | `test_minimize_max_sum_per` |
| `MAXIMIZE MIN(SUM(...)) PER col` | `test_maximize_min_sum_per` |
| Plus MAX(AVG), MIN(AVG), SUM(AVG) nested variants | `test_*_*_avg_per` |

### Error cases

| Scenario | Where |
|----------|-------|
| `MAX(x) <> K` rejected | `test_min_max.py::test_max_notequal_error` |
| Flat `MIN/MAX + PER` (ambiguous) rejected | `test_per_objective.py` |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| MIN/MAX (easy) | WHEN | ✓ |
| MIN/MAX (hard) | aggregate-local WHEN | ✓ |
| MIN/MAX (easy) | PER (stripped) | ✓ |
| MIN/MAX (easy) | WHEN + PER | ✓ |
| MIN/MAX (nested) | PER (objective) | ✓ |
| MIN/MAX (easy) | entity-scoped | ✓ |
| MIN/MAX (hard) | entity-scoped | ✓ |
| MIN/MAX | INTEGER variables | ✓ |
| MIN/MAX (hard) | PER (per-group Big-M) | ✓ |
| MIN/MAX | multiple constraints in same query | ✓ |
| MIN/MAX | constraint + objective in same query | ✓ |

### Composed MIN/MAX (additive LHS/objective with mixed aggregate terms)

| Scenario | Test |
|----------|------|
| `SUM(x*v) + MAX(x*v) WHEN w <= K` (constraint, easy) | `test_sum_plus_max_leq_composed` |
| `MAX(...) WHEN w1 + MAX(...) WHEN w2 <= K` (two easy MAXs) | `test_max_plus_max_leq_composed` |
| `MIN(...) WHEN w1 + MIN(...) WHEN w2 >= K` (two easy MINs) | `test_min_plus_min_geq_composed` |
| `MINIMIZE SUM(x*v) + MAX(x*v) WHEN w` (easy objective) | `test_minimize_sum_plus_max_composed_objective` |
| `MAXIMIZE MIN(x*v) WHEN w + SUM(x*v)` (easy objective) | `test_maximize_min_plus_sum_composed_objective` |

### Composed MIN/MAX rejection (binder errors)

| Scenario | Test |
|----------|------|
| Hard direction (e.g. `SUM + MAX >= K`) | `test_composed_minmax_hard_rejected` |
| Subtraction (`MAX - MIN <= K`) | `test_composed_minmax_subtraction_rejected` |
| Scalar multiplication (`2 * MIN(...) + SUM(...)`) | `test_composed_minmax_scalar_mult_rejected` |
| Outer `PER` wrapper on composed constraint | `test_composed_minmax_per_wrapper_rejected` |
| Hard-direction composed objective | `test_composed_minmax_objective_hard_rejected` |

### Empty `WHEN` rejection (execution-time errors)

Previously tracked as a bug in `todo.md`: empty-WHEN on hard-direction MIN/MAX silently floated the `z`/`z_k` auxiliary, making constraints vacuous and objectives meaningless. Now rejected pre-solver by `RejectEmptyAggregate` in `physical_decide.cpp`. See `03_expressivity/when/done.md` → "Empty Row Sets" for the full rule.

| Scenario | Test |
|----------|------|
| `MAXIMIZE MIN(...) WHEN empty` and MAX/hard mirrors | `test_maximize_min_objective_when_empty` + 3 siblings |
| `MAX(...) WHEN empty >= K` (hard constraint) | `test_max_when_empty_constraint_hard` |
| `MIN(...) WHEN empty <= K` (hard constraint) | `test_min_when_empty_constraint_hard` |
| Easy direction: `MAX(...) <= K WHEN empty`, `MIN(...) >= K WHEN empty` | `test_max_leq_constraint_when_empty`, `test_min_geq_constraint_when_empty` + mirrors |
| Composed `SUM + (MAX WHEN empty)` | `test_sum_plus_max_when_empty_silently_vacates_constraint`, `test_composed_easy_min_when_empty_rejected` |
| Mixed empty+populated aggregate-local WHEN terms | `test_mixed_empty_and_populated_when_terms_constraint` / `_objective` |
| SUM / AVG empty WHEN | `test_sum_when_empty_rejected`, `test_avg_when_empty_rejected`, `test_avg_constraint_when_filters_all_rows` |
| PER with one empty group (skip preserved) | `test_avg_per_constraint_with_empty_group` |
