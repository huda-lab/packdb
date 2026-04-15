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
| `MAX(expr) <= K` → per-row | `test_min_max.py::test_max_constraint_easy` | ✓ |
| `MIN(expr) >= K` → per-row | `test_min_max.py::test_min_constraint_easy` | ✓ |
| `MAX + WHEN` (easy) | `test_min_max.py::test_max_constraint_with_when` | ✓ |
| `MAX + PER` (stripped as redundant) | `test_min_max.py` | ✓ |
| `MAX <= K` with entity-scoped | `test_entity_scope.py::test_entity_scoped_with_max` | ✓ |
| `MIN >= K` with entity-scoped | `test_entity_scope.py::test_entity_scoped_min_easy_case` | ✓ |
| Aggregate-local WHEN on MAX (easy) | `test_aggregate_local_when.py::test_aggregate_local_when_with_max` | ✓ |

### Hard cases (Big-M indicators)

| Scenario | Where | Oracle |
|----------|-------|--------|
| `MAX(expr) >= K` (hard) | `test_min_max.py` | ✓ |
| `MIN(expr) <= K` (hard) | `test_min_max.py` | ✓ |
| `MAX(expr) = K` (equality) | `test_min_max.py` | ✓ |
| `MIN(expr) = K` (equality) | `test_min_max.py` | ✓ |
| `MAX >= K` with entity-scoped | `test_entity_scope.py::test_entity_scoped_max_hard_case` | ✓ |
| `MAX(expr) >= K PER col` (Big-M per group) | `test_per_interactions.py::test_per_max_geq_constraint` | ✓ |
| `MIN(expr) <= K PER col` (Big-M per group) | `test_per_interactions.py::test_per_min_leq_constraint` | ✓ |
| `MAX(expr) = K PER col` (easy + hard combined per group) | `test_per_interactions.py::test_per_max_eq_constraint` | ✓ |

### Objectives

| Scenario | Where | Oracle |
|----------|-------|--------|
| `MINIMIZE MAX(expr)` (easy) | `test_min_max.py::test_min_objective_with_when` | ✓ |
| `MAXIMIZE MIN(expr)` (easy) | `test_min_max.py` | ✓ |
| `MAXIMIZE MAX(expr)` (hard) | `test_min_max.py` | ✓ |
| `MINIMIZE MIN(expr)` (hard) | `test_min_max.py` | ✓ |

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
| `MAX(x) <> K` rejected | `test_min_max.py` |
| Flat `MIN/MAX + PER` (ambiguous) rejected | `test_per_objective.py` |

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| MIN/MAX (easy) | WHEN | ✓ |
| MIN/MAX (hard) | WHEN | ✓ (via aggregate-local for easy; hard+WHEN via `test_min_max.py`) |
| MIN/MAX (easy) | PER (stripped) | ✓ |
| MIN/MAX (nested) | PER (objective) | ✓ |
| MIN/MAX (easy) | entity-scoped | ✓ |
| MIN/MAX (hard) | entity-scoped | ✓ |
| MIN/MAX | INTEGER variables | ✓ |
| MIN/MAX (aggregate-local WHEN, easy) | — | ✓ |
| MIN/MAX (hard) | PER (per-group Big-M) | ✓ (`test_per_interactions.py`) |
