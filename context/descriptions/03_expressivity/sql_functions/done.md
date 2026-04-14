# SQL Functions & Expressions — Implemented Features

This file documents which SQL functions and expressions work inside DECIQL clauses today.

---

## Aggregate Functions

### SUM()

The primary aggregate over expressions involving decision variables. Valid in both constraints and objectives.

```sql
MAXIMIZE SUM(x * value)
SUCH THAT SUM(x * weight) <= 50
```

**Code**: Validated in `decide_objective_binder.cpp` and `decide_constraints_binder.cpp` — aggregates other than SUM, AVG, MIN, and MAX are rejected with an error.

### COUNT() — BOOLEAN and INTEGER variables

`COUNT(x)` counts the number of rows where the decision variable `x` is non-zero. It is supported for both BOOLEAN and INTEGER variables (REAL is not yet supported).

**BOOLEAN variables**: `COUNT(x)` is rewritten to `SUM(x)` directly, since for a {0,1} variable, "how many rows have x=1" equals `SUM(x)`.

**INTEGER variables**: `COUNT(x)` uses the Big-M indicator variable technique:
1. A hidden binary indicator variable `z` is introduced for `x`
2. Two linking constraints enforce the relationship: `z <= x` (z=0 when x=0) and `x <= M*z` (z=1 when x>0)
3. `COUNT(x)` is rewritten to `SUM(z)`, which counts non-zero assignments
4. M is derived from the upper bound of `x` (defaults to 1e6 if no bound specified)

```sql
SUCH THAT COUNT(x) >= 5     -- where x IS BOOLEAN or INTEGER
MAXIMIZE COUNT(x)           -- maximize number of non-zero rows
SUCH THAT COUNT(x) <= 2     -- at most 2 non-zero assignments
```

The binder recognizes COUNT as a valid DECIDE aggregate and binds it into a `BoundAggregateExpression`. The rewrite to SUM (with indicator variable creation for INTEGER variables) is performed by `DecideOptimizer::RewriteCountToSum` in `decide_optimizer.cpp`. This means COUNT inherits all SUM capabilities (WHEN, PER, all comparison operators) for free. Multiple `COUNT(x)` references to the same variable reuse a single indicator.

`COUNT(x)` is **rejected** for REAL variables with a clear error message.

---

### AVG() — Coefficient Scaling at Execution Time

`AVG(expr)` over decision variables is treated as an aggregate constraint like SUM, but terms are scaled by the row count N at execution time so the model represents the average, not the raw sum.

**Semantics**: Standard SQL AVG — divide by count of all rows (decision variables are never NULL). This is always linear since N is a data-determined constant.

**Constraints**: Semantically, `AVG(expr) op K` is equivalent to `SUM(expr) op K*N` where N depends on context:
- No WHEN/PER: N = total row count
- WHEN: N = count of WHEN-matching rows
- PER: N = count of rows in each group
- WHEN+PER: N = count of WHEN-matching rows per group
- Aggregate-local WHEN: N = count of rows matching that aggregate-local filter, within each PER group if PER is present

**Objectives (flat)**: `MAXIMIZE/MINIMIZE AVG(expr)` uses the same optimal assignment as `SUM(expr)` when there is one global denominator. In mixed additive aggregate expressions, PackDB preserves AVG scaling per term so `AVG(a) + SUM(b)` is not treated as `SUM(a) + SUM(b)`.

**Objectives (nested PER)**: `OUTER(AVG(expr)) PER col` is fully supported. Inner AVG scales each row's coefficient by `1/n_g` (group size), producing true per-group averages. Outer AVG maps to SUM (dividing by constant G). See [maximize_minimize/done.md](../maximize_minimize/done.md).

```sql
SUCH THAT AVG(x * weight) <= 10         -- SUM(x*weight) <= 10*N
SUCH THAT AVG(x) <= 0.5                 -- at most half the rows selected (BOOLEAN)
SUCH THAT AVG(x * cost) <= 5 WHEN active -- only among active rows
SUCH THAT AVG(x * cost) WHEN active + SUM(x * fee) WHEN priority <= 100
SUCH THAT AVG(x * hours) <= 8 PER emp   -- per-group average
MAXIMIZE AVG(x * profit)                -- same as MAXIMIZE SUM(x * profit)
```

**Code**: AVG flows through binding natively (no parse-time rewrite), preserving its DOUBLE return type so fractional RHS values survive type coercion. The binders (`decide_constraints_binder.cpp`, `decide_objective_binder.cpp`) accept `"avg"` alongside `"sum"`. The `DecideOptimizer` rewrites AVG to SUM while tagging the aggregate with `AVG_REWRITE_TAG`. At execution time (`physical_decide.cpp`), expression analysis marks extracted terms with `avg_scale`; coefficient evaluation scales linear and bilinear terms by `1/N`, and quadratic inner terms by `1/sqrt(N)`.

**Tests**: `test/decide/tests/test_avg.py` — 9 test cases covering objectives, constraints, WHEN, PER, WHEN+PER, BOOLEAN, INTEGER, non-linear rejection, and no-decide-var passthrough.

---

### MIN() / MAX() — Per-Row and Big-M Indicator Rewrites

`MIN(expr)` and `MAX(expr)` over decision variables are supported in both SUCH THAT constraints and MAXIMIZE/MINIMIZE objectives. The implementation strategy depends on whether the case is "easy" (naturally per-row) or "hard" (requires Big-M indicator variables and a global auxiliary variable).

#### Easy Constraint Cases (No Big-M)

When the comparison direction already bounds each row individually, no auxiliary variables are needed:

- `MAX(expr) <= K` → per-row constraint: `expr <= K` for every row
- `MIN(expr) >= K` → per-row constraint: `expr >= K` for every row

These are trivially correct: bounding every row satisfies the aggregate bound. PER on easy cases is stripped (redundant, since constraints are already per-row).

#### Hard Constraint Cases (Big-M Indicators)

When the aggregate must be tight (equality or the "wrong" direction), a global auxiliary variable `z` and per-row binary indicators are introduced:

- `MAX(expr) >= K` → global variable `z >= K`, per-row: `z >= expr`, plus Big-M indicators ensuring `z` equals some row's value
- `MIN(expr) <= K` → global variable `z <= K`, per-row: `z <= expr`, plus Big-M indicators
- Equality cases (`MAX(expr) = K`, `MIN(expr) = K`) → both directions constrained

#### Objective Cases

- **Easy objectives**: `MINIMIZE MAX(expr)` and `MAXIMIZE MIN(expr)` — a single global auxiliary variable `z` with per-row linking constraints (`z >= expr_i` for MAX, `z <= expr_i` for MIN). The objective optimizes `z` directly.
- **Hard objectives**: `MAXIMIZE MAX(expr)` and `MINIMIZE MIN(expr)` — requires `z` plus per-row binary indicator variables to ensure `z` equals some row's actual value (Big-M formulation).

#### Composition

- **WHEN**: Composes naturally. WHEN masks filter which rows participate in the MIN/MAX aggregate, and constraint/indicator generation skips non-matching rows.
- **PER (easy cases)**: Stripped as redundant — easy cases already produce per-row constraints.

```sql
-- Easy constraint cases (no Big-M)
SUCH THAT MAX(x * cost) <= 100         -- per-row: x*cost <= 100
SUCH THAT MIN(x * hours) >= 2          -- per-row: x*hours >= 2

-- Hard constraint cases (Big-M indicators)
SUCH THAT MAX(x * cost) >= 50          -- global z, binary indicators
SUCH THAT MIN(x * hours) = 4           -- equality: both directions

-- Objectives
MINIMIZE MAX(x * cost)                 -- easy: global z, minimize
MAXIMIZE MIN(x * profit)               -- easy: global z, maximize
MAXIMIZE MAX(x * profit)               -- hard: z + binary indicators
MINIMIZE MIN(x * cost)                 -- hard: z + binary indicators

-- With WHEN
SUCH THAT MAX(x * cost) <= 50 WHEN category = 'electronics'
MINIMIZE MAX(x * deviation) WHEN active = 1
```

**Code**: The rewrite is performed by `DecideOptimizer::RewriteMinMaxConstraints` and `DecideOptimizer::RewriteMinMaxInConstraint` in `decide_optimizer.cpp`, which detect MIN/MAX aggregates and insert `__MIN__`/`__MAX__` marker tags into the symbolic representation. The binders (`decide_constraints_binder.cpp`, `decide_objective_binder.cpp`) whitelist MIN/MAX alongside SUM and AVG. The symbolic layer (`decide_symbolic.cpp`) recognizes the `__MIN__`/`__MAX__` markers. At execution time, `physical_decide.cpp` generates the appropriate per-row constraints, Big-M indicator constraints, and global auxiliary variables. Global variable and constraint support is provided by `solver_input.hpp` and `ilp_model_builder.cpp`.

---

## ABS() — Linearized Automatically

`ABS(expr)` over decision variables is automatically linearized using the standard ILP technique. For each `ABS(expr)` that references a DECIDE variable, the system:

1. Introduces an auxiliary REAL variable `d` (hidden from query output)
2. Adds two constraints: `d >= expr` and `d >= -expr`
3. Replaces `ABS(expr)` with `d`

This works in both constraints and objectives:

```sql
-- In objectives: minimize total absolute deviation
MINIMIZE SUM(ABS(new_hours - hours))

-- In per-row constraints: bound deviation per row
SUCH THAT ABS(new_qty - l_quantity) <= 5

-- In aggregate constraints: bound total deviation
SUCH THAT SUM(ABS(new_qty - l_quantity)) <= 50
```

`ABS()` without decision variables (e.g., `ABS(col1 - col2)`) is left as regular SQL — no rewrite occurs.

**Code**: The rewrite is performed by `DecideOptimizer::RewriteAbs` in `decide_optimizer.cpp`. The binder binds ABS as a normal function; the optimizer detects it, creates auxiliary variables, and generates linearization constraints. Auxiliary variables are hidden from `SELECT *` by truncating the bind context.

**Tests**: `test/decide/tests/test_abs_linearization.py` — 8 test cases covering objectives, constraints, WHEN, PER, multiple ABS terms, no-decide-var, and mixed variable types.

---

## Arithmetic Operators

### Multiplication (`*`) — variable x constant or variable x column

```sql
x * 5              -- OK: variable * literal
x * weight         -- OK: variable * column (constant per row)
SUM(x * weight)    -- OK: aggregate of linear product
```

`x * y` (variable x variable) is **not supported** (non-linear).

### Addition / Subtraction (`+`, `-`)

```sql
x + y              -- OK: sum of variables
SUM(x * a + y * b) -- OK: linear combination in aggregate
```

---

## Comparison Operators

### =, <, <=, >, >=

Five standard comparison operators are supported in constraints.

```sql
SUCH THAT x <= 1
SUCH THAT SUM(x * w) >= 10
SUCH THAT SUM(x) = 5
```

`<>` (not-equal) is supported on both per-row and aggregate constraints. It uses Big-M disjunction with an auxiliary binary indicator variable:

- `SUM(x) <> K` → two constraints: `SUM(x) <= K-1 + M*z` and `SUM(x) >= K+1 - M*(1-z)`
- `x <> K` → same pattern applied per-row

**Complexity**: Adds 1 binary variable and 2 constraints per `<>`. The Big-M value M is computed from variable bounds at execution time. Loose bounds produce weaker LP relaxations.

**Code**: Auxiliary indicator variable created by `DecideOptimizer::RewriteNotEqual` in `decide_optimizer.cpp`; Big-M constraints generated at execution time in `physical_decide.cpp` (`Finalize()`), where data bounds are available.

### BETWEEN ... AND ...

Desugars to `>= lower AND <= upper`. Produces two constraints.

```sql
SUCH THAT SUM(x) BETWEEN 10 AND 50
-- equivalent to: SUM(x) >= 10 AND SUM(x) <= 50
```

### IN (...)

Constrains a value to be in a literal set. Works on both table columns and decision variables.

```sql
SUCH THAT category IN ('A', 'B', 'C')   -- table column (SQL filter)
SUCH THAT x IN (0, 1, 3)                -- decision variable domain restriction
```

**Decision variable IN**: `x IN (v1, ..., vK)` is rewritten at bind time into K auxiliary binary indicator variables with two constraints:
- Cardinality: `z_1 + ... + z_K = 1` (exactly one value selected)
- Linking: `x = v1*z_1 + ... + vK*z_K` (x takes the selected value)

**Complexity**: Adds K binary variables and 2 constraints per IN. For small K (2–5 values) this is cheap. Large K (e.g., 100 values) adds significant model size — consider whether the domain can be expressed as a range constraint instead.

**Optimizations**:
- `x IN (0, 1)` on BOOLEAN → trivially satisfied, no rewrite
- `x IN (v)` single value → rewritten to `x = v`

**Code**: `bind_select_node.cpp` (`RewriteInDomain()`), called before constraint binding. IN on aggregates (e.g., `SUM(x) IN (...)`) remains unsupported.

---

## NULL-Related Expressions

### IS NULL / IS NOT NULL

Supported in WHEN conditions and WHERE clause (not over decision variables).

```sql
imputed_distance = distance WHEN distance IS NOT NULL AND
imputed_distance <= 10 WHEN (distance IS NULL AND mode = 'walk-bike')
```

---

## Boolean / Logical Operators

### AND — Two Roles

1. **Constraint separator** at the top level of `SUCH THAT`
2. **Logical AND** inside a `WHEN` condition (requires parentheses)

### OR

Valid in `WHEN` conditions and `WHERE` only. Not supported as a constraint combiner over decision variables (would create non-linear disjunctions).

---

## Summary Table (Implemented Only)

| Function / Operator | In Constraints | In Objective | In WHEN / WHERE |
|---|---|---|---|
| `SUM()` over dec. vars | Yes | Yes | N/A |
| `AVG()` over dec. vars | Yes (RHS scaled) | Yes (→SUM) | N/A |
| `COUNT()` (BOOLEAN, INTEGER) | Yes | Yes | N/A |
| `MIN()` / `MAX()` over dec. vars | Yes (per-row / Big-M) | Yes (global aux / Big-M) | N/A |
| `ABS()` over dec. vars | Yes (linearized) | Yes (linearized) | N/A |
| `*` (var x const/col) | Yes | Yes | N/A |
| `+`, `-` | Yes | Yes | Yes |
| `=`, `<`, `<=`, `>`, `>=` | Yes | N/A | Yes |
| `<>` (not-equal) | Yes (Big-M) | N/A | Yes |
| `BETWEEN` | Yes | N/A | Yes |
| `IN (...)` | Yes (dec. vars + columns) | N/A | Yes |
| `IS NULL` / `IS NOT NULL` | N/A | N/A | Yes |
| `AND` (constraint sep.) | Yes | N/A | N/A |
| `AND` / `OR` (logical) | N/A | N/A | Yes |
