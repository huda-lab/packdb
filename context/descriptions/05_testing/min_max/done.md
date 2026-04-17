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
