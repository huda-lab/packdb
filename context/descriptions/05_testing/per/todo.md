# PER Clause Test Coverage — Todo

PER is the weakest interaction partner in the test suite: its combinations with
bilinear, ABS (aggregate constraint), COUNT(INTEGER), and QP objectives all have
zero coverage. Each involves auxiliary variable / indicator creation that must
be correctly partitioned by group — a class of bugs that would produce silent
wrong results.

## Missing coverage

### HIGH: PER + bilinear

See [bilinear/todo.md](../bilinear/todo.md) for full details. This gap is the
single largest cross-feature hole.

### HIGH: PER + ABS in aggregate constraint

See [abs/todo.md](../abs/todo.md). ABS auxiliary variables must be partitioned
by PER group.

### HIGH: PER + COUNT(x INTEGER)

See [count/todo.md](../count/todo.md). Big-M indicator variable creation +
PER group partitioning.

### HIGH: PER + QP objective

See [quadratic/todo.md](../quadratic/todo.md). Q matrix construction with PER
group auxiliaries.

### HIGH: PER + hard MIN/MAX constraints

See [min_max/todo.md](../min_max/todo.md). Big-M indicators must be created
per-group.

### HIGH: Multiple variables + PER

No test exercises multi-variable coefficient extraction with PER grouping. Each variable's coefficients must be correctly partitioned by group, and the variable-indexing layer must produce the right column mapping for each group's constraint.

```sql
-- Multiple variables + PER
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, 10 AS w UNION ALL
    SELECT 2, 'A', 5 UNION ALL
    SELECT 3, 'B', 8 UNION ALL
    SELECT 4, 'B', 3
)
SELECT id, grp, x, y FROM data
DECIDE x IS BOOLEAN, y IS INTEGER
SUCH THAT SUM(x * w) <= 12 PER grp AND y <= 3 AND SUM(y) <= 8
MAXIMIZE SUM(x * w + y)
```

### HIGH: WHEN + PER + multiple variables (triple)

Multi-variable coefficient paths under combined WHEN filtering + PER grouping. Each variable's coefficients in each group must respect the WHEN mask independently.

```sql
-- Triple: WHEN + PER + multiple vars
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, true AS active, 10 AS w, 5 AS v UNION ALL
    SELECT 2, 'A', false, 5, 8 UNION ALL
    SELECT 3, 'B', true, 8, 3 UNION ALL
    SELECT 4, 'B', true, 3, 9
)
SELECT id, grp, x, y FROM data
DECIDE x IS BOOLEAN, y IS INTEGER
SUCH THAT SUM(x * w + y * v) <= 30 WHEN active PER grp
MAXIMIZE SUM(x * w + y * v)
```

### MEDIUM: Feasibility problem with PER

`DECIDE x IS BOOLEAN SUCH THAT SUM(x) = 1 PER col` without an objective. The model builder sets all objective coefficients to zero; constraint generation under PER must still work correctly with a `DecideSense::FEASIBILITY` sense.

```sql
-- Feasibility + PER
SELECT id, grp, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) = 1 PER grp
```

### MEDIUM: PER equality constraint

`SUM(x) = K PER col` generates two-sided bounds per group (both `>=` and `<=`). Only one-sided PER constraints are tested.

```sql
-- PER equality
SELECT id, grp, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) = 2 PER grp
MAXIMIZE SUM(x * val)
```

### MEDIUM: PER on per-row constraint rejection (error test)

Documented restriction: "PER requires an aggregate constraint." No test verifies the error message when `x <= 5 PER col` is attempted. Users could accidentally write this, and error message quality matters.

```sql
-- Should be rejected
SUCH THAT x <= 5 PER grp
```

### LOW: PER with zero-coefficient groups

Some PER groups where `SUM(x * col)` has all-zero coefficients (e.g., all `col` values are 0 in one group). The constraint becomes trivially satisfied for that group; the constraint-builder path for zero-row contributions within a non-empty group is a subtle edge case.

### LOW: Single-row PER groups

Data where each PER group has exactly one row. Degenerates to per-row bounds; the coefficient vector has a single entry per group. Tests the group-indexing path's behavior with `|group| = 1`.

### LOW: NULL in PER column AND WHEN condition simultaneously

Each is tested independently, but not together. A row with NULL PER key that passes the WHEN condition could cause an off-by-one or incorrect group count.

```sql
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, 10.0 AS val, true AS active UNION ALL
    SELECT 2, NULL, 5.0, true UNION ALL
    SELECT 3, 'B', 8.0, false
)
SELECT id, grp, val, active, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * val) <= 10 WHEN active PER grp
MAXIMIZE SUM(x * val)
```

### LOW: Uncorrelated subquery in PER constraint RHS

`SUM(x) <= (SELECT AVG(col) FROM other_table) PER group` — the scalar RHS must be shared across all PER groups. If execution evaluates the subquery per-group, errors could occur. Positive test missing (only the error case for non-scalar RHS is tested).

```sql
SELECT l_orderkey, l_linenumber, l_extendedprice, x
FROM lineitem WHERE l_orderkey <= 5
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_extendedprice) <= (SELECT 10000) PER l_orderkey
MAXIMIZE SUM(x)
```

## Not yet tested but low priority: PER STRICT

The `PER STRICT` modifier (per-constraint/objective switch from WHEN→PER to
PER→WHEN) was recently added. Coverage is needed across:
- PER STRICT + WHEN (vacuously true upper bound)
- PER STRICT + WHEN (infeasible lower bound)
- PER STRICT + hard MIN/MAX (existential direction → infeasible)
- PER STRICT + easy MIN/MAX (no-op)
- PER STRICT + entity-scoped (currently xfail in `test_entity_scope.py` — parser issue)

## Cross-references

- `WHEN + PER` triples in `when/done.md`
- `PER + entity_scope` combinations in `entity_scope/done.md`
