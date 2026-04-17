# Variable Type Test Coverage — Todo

## Closed

- **IS REAL with MINIMIZE objective** — `test_var_real.py::test_real_minimize` (2026-04-17). Oracle-compared LP with `SUM(x) >= 10` lower bound forcing a non-zero optimum; MINIMIZE pushes the load onto the smallest-coefficient row. Exercises the coefficient-sign path that's distinct from MAXIMIZE (sign flip happens in the objective builder or solver adapter).

## Missing coverage

### MEDIUM: INTEGER + REAL without BOOLEAN

`DECIDE x IS INTEGER, y IS REAL` is never tested as a pair. MILP variable type flags (`is_integer`, `is_binary`) must be set correctly when no BOOLEAN variables are present — `is_binary=false` for all, `is_integer` mixed. An off-by-one in the flag-setting loop could leave an INTEGER variable as continuous without this test catching it.

```sql
SELECT id, x, ROUND(y, 2) AS y FROM data
DECIDE x IS INTEGER, y IS REAL
SUCH THAT x <= 5 AND y <= 10 AND SUM(x + y) <= 20
MAXIMIZE SUM(x * val_a + y * val_b)
```

### MEDIUM: Three or more decision variables

No test uses more than 2 decision variables. Variable indexing in `physical_decide.cpp` and `VarIndexer` could have off-by-one errors only visible with 3+ variables (e.g., if a loop iterates `i < 2` hard-coded anywhere).

```sql
SELECT id, x, y, ROUND(z, 2) AS z FROM data
DECIDE x IS BOOLEAN, y IS INTEGER, z IS REAL
SUCH THAT SUM(x) <= 3 AND y <= 5 AND z <= 10.0
    AND SUM(x * a + y * b + z * c) <= 100
MAXIMIZE SUM(x * val_a + y * val_b + z * val_c)
```

### LOW: Fractional solution verification for IS REAL

No test forces and verifies a genuinely non-integer REAL result (e.g., `SUM(x) = 10` over 3 rows producing x = 3.333...). Every existing REAL test could pass even if the solver silently returned integer-valued solutions. If REAL variables were truncated or rounded in the readback path (`physical_decide.cpp` DOUBLE output), no test would catch it.

```sql
-- Force a fractional optimal
WITH data AS (
    SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
)
SELECT id, ROUND(x, 6) AS x FROM data
DECIDE x IS REAL
SUCH THAT x <= 5 AND SUM(x) = 10
MAXIMIZE SUM(x)
-- Expected x = 10/3 ≈ 3.333333 for all rows
```

### LOW: Mixed types in same constraint/objective (explicit)

`SUM(x * col + y * col2) <= K` where `x IS BOOLEAN, y IS REAL` — tested implicitly through ABS (which creates REAL auxiliary variables alongside user BOOLEAN variables), but never tested explicitly with user-declared mixed types in the same aggregate term.

## Cross-references

- `IS REAL + entity-scoped` — see [entity_scope/todo.md](../entity_scope/todo.md)
- `IS REAL + per-row WHEN` — see [when/todo.md](../when/todo.md)
