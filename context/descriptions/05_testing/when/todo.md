# WHEN Clause Test Coverage — Todo

## Closed

- **Hard MIN/MAX + aggregate-local WHEN** — `test_aggregate_local_when.py::test_aggregate_local_when_with_hard_max` (2026-04-17). The hard-direction `MAX(x * value) WHEN active >= 6` is oracle-compared; Big-M indicators are emitted only for WHEN-matching rows, and non-matching rows are unconstrained by the aggregate. See also `min_max/done.md`.

## Missing coverage

### MEDIUM: WHEN on per-row constraint with IS REAL

All per-row WHEN tests use BOOLEAN or INTEGER variables. REAL variables have different bound semantics (no implicit [0,1] cap), so the WHEN skip-constraint path for continuous variables is untested.

```sql
-- Per-row WHEN with REAL variable
SELECT id, ROUND(x, 2) AS x, active FROM data
DECIDE x IS REAL
SUCH THAT x <= 10 WHEN active AND SUM(x) <= 30
MAXIMIZE SUM(x * profit)
```

### MEDIUM: WHEN on objective matching zero rows

When the *objective* WHEN filters out all rows, every coefficient in the objective vector becomes 0. The constraint path (`test_when_no_rows_match`) is tested, but the objective path (objective vector construction) is distinct and could fail differently.

```sql
-- Objective WHEN with zero-match
SELECT id, val, flag, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 1
MAXIMIZE SUM(x * val) WHEN flag = 'NONEXISTENT'
```

### MEDIUM: `WHEN col IS NULL` / `WHEN col IS NOT NULL` as condition syntax

`test_when_null_condition_column` tests NULL *values* in the filtered column (where the condition evaluates to NULL implicitly), but no test uses the `IS NULL` / `IS NOT NULL` predicate explicitly in the WHEN condition. A grammar or binder defect here would go undetected.

```sql
-- Explicit IS NOT NULL predicate in WHEN
SELECT id, x, distance FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 3
MAXIMIZE SUM(x * value) WHEN distance IS NOT NULL
```

