# Entity-Scope Test Coverage — Todo

## Upgrade constraint-only tests to oracle

These tests currently verify constraints but not optimality. Remaining items:

| Test | Gap | Oracle strategy |
|------|-----|-----------------|
| `test_entity_scoped_integer_count` | COUNT(INTEGER) Big-M — no objective check | Hard: oracle needs Big-M indicator vars per nation to model COUNT |
| `test_entity_scoped_mixed_when_per` | All-four interaction — constraint-only | Hard: 1500-customer MIP exceeds HiGHS reliability; oracle finds sub-optimal (74993 vs PackDB 74995). Needs Gurobi or problem size reduction to add oracle. |
| `test_entity_scoped_ne_per` | NE + PER — constraint-only | Oracle: hard; NE with PER needs Big-M per group |

Previously completed (oracle added):
- `test_entity_scoped_with_max` — ✓ oracle verifies MAX easy case eligibility
- `test_entity_scoped_max_hard_case` — ✓ oracle verifies MAX >= K qualifying nation constraint
- `test_entity_scoped_min_easy_case` — ✓ oracle verifies MIN easy case eligibility (all blocked on sf=0.01)
- `test_entity_scoped_avg_per` — ✓ oracle verifies per-region AVG scaling
- `test_entity_scoped_two_tables` — ✓ oracle added; also fixed degenerate SUM(keepR) <= 3 → <= 10
- `test_entity_scoped_when_entity_invisible` — ✓ oracle confirms trivially optimal (all filtered, SUM=0)

## Missing feature combinations

| Gap | Risk | Suggested test |
|-----|------|---------------|
| PER STRICT + entity-scoped | HIGH — STRICT changes WHEN→PER order, currently xfail (parser broken) | `test_entity_scoped_per_strict` — unxfail when parser fixed |
| entity-scoped + bilinear | HIGH — entity dedup + McCormick auxiliary variable indexing untested | See below |
| IS REAL + entity-scoped (in entity_scope file) | MEDIUM — DOUBLE readback via VarIndexer entity mapping untested here | Add basic IS REAL entity-scoped test |
| entity-scoped + hard MIN/MAX | MEDIUM — Big-M indicator indexing must account for entity dedup | See below |
| entity-scoped + `<>` (NE) | MEDIUM — NE indicator rewrite with entity-keyed variables | See below |
| entity-scoped + WHEN + MIN/MAX (triple) | MEDIUM — each pair tested, triple untested | See below |
| ABS linearization + entity-scoped | MEDIUM — ABS rewrite may not handle entity-scoped coefficient assembly | `SUM(ABS(keepN * acctbal - 500)) <= K` style constraint |
| Subquery RHS + entity-scoped + PER | MEDIUM — three-way interaction untested | `SUM(keepN) <= (SELECT ...) PER r_name` |
| JOIN fan-out with entity-scoped | MEDIUM — duplicate rows after join could affect entity key dedup | Test with a many-to-many join producing multiple rows per entity |
| NULL in entity key column | LOW — NULL grouping in VarIndexer could silently create wrong entity | `WHERE n_nationkey IS NOT NULL` should be robust; test without filter |
| entity_scope + QP objective | LOW — covered in `test_quadratic_constraints.py` | Verify POWER(keepN, 2) and keepN * x work with entity-scoped |

## HIGH-risk gap details

### entity-scoped + bilinear

No test combines table-scoped variables with bilinear terms. Entity deduplication means multiple result rows share one solver variable. McCormick auxiliary variable indexing must use entity-keyed indices, not row indices. A mismatch produces wrong linking constraints.

```sql
-- Entity-scoped variable with bilinear
SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN, ROUND(x, 2) AS x
FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
WHERE n.n_regionkey = 0 AND n.n_nationkey <= 5
DECIDE n.keepN IS BOOLEAN, x IS REAL
SUCH THAT x <= 100 AND SUM(keepN * x) <= 500
MAXIMIZE SUM(keepN * x * c_acctbal)
```

## MEDIUM-risk gap details

### Entity-scoped IS REAL

`test_entity_scope.py` covers BOOLEAN (tests 1, 2, 5) and INTEGER (test 3) but never REAL. The DOUBLE readback through `VarIndexer` entity mapping is untested here (covered tangentially in `test_quadratic_constraints.py::test_table_scoped_variables`, but without a standalone LP test to pin down the readback path). A type mismatch in the entity-keyed variable path would produce garbage values.

```sql
SELECT n_nationkey, ROUND(budget, 2) AS budget
FROM nation WHERE n_regionkey <= 2
DECIDE nation.budget IS REAL
SUCH THAT budget <= 1000 AND SUM(budget) <= 5000
MAXIMIZE SUM(budget * n_nationkey)
```

### Entity-scoped with hard MIN/MAX

Only easy cases (`MAX <= K`, `MIN >= K`) and one hard case (`MAX >= K`) are tested. Other hard cases (`MIN(expr) <= K`, equality) have no entity-scoped coverage. Indicator indexing must account for entity dedup across all hard-case variants.

```sql
SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN
FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
WHERE n.n_regionkey = 0
DECIDE n.keepN IS BOOLEAN
SUCH THAT MIN(keepN * c_acctbal) <= 100
MAXIMIZE SUM(keepN)
```

### Entity-scoped with `<>`

NE (`<>`) + entity-scoped is tested in `test_entity_scoped_ne_constraint` (constraint-only) and `test_entity_scoped_ne_per` (constraint-only, NE + PER). Both lack oracle verification. Upgrade to oracle: construct the Big-M disjunction manually for the entity-deduplicated variable.

### Entity-scoped + WHEN + MIN/MAX (triple)

Each pair (entity + WHEN, entity + MIN/MAX, WHEN + MIN/MAX) is tested separately, but all three together are not. Entity dedup + WHEN mask + MIN/MAX indicator generation must compose correctly.

```sql
SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN, active
FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
WHERE n.n_regionkey = 0
DECIDE n.keepN IS BOOLEAN
SUCH THAT MAX(keepN * c_acctbal) >= 5000 WHEN active
MAXIMIZE SUM(keepN)
```

## Infrastructure

- [ ] Add `perf_tracker.record()` calls to oracle-verified tests (currently only Test 1 passes `perf_tracker` to record timing).
- [ ] Tests 12–26 (new tests) were written without `perf_tracker` in signature; add it if performance tracking of new tests is wanted.
