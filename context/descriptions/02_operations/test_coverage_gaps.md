# Test Coverage Gaps

Prioritized list of gaps in the DECIDE test suite, discovered via systematic audit of the full expressivity surface against existing tests in `test/decide/tests/`.

**Last audited**: 2026-04-14
**Tests audited**: ~150+ tests across 39 files
**Methodology**: Three-axis audit — (1) single-feature coverage, (2) cross-feature interactions, (3) edge cases and data shapes. Each gap is classified by what kind of bug could hide there.

For how the testing framework works, see [oracle.md](oracle.md).

---

## How to Use This Document

1. When writing new tests, check here first for the highest-risk gaps.
2. When implementing a new feature, check whether it creates new interaction gaps.
3. After closing a gap, remove or strike through the entry and note which test covers it.
4. Re-audit periodically (use `/test-review` to regenerate).

---

## Risk Levels

| Level | Meaning | Examples |
|-------|---------|---------|
| **HIGH** | Missing test for a rewrite, linearization, or optimizer interaction. Silent math errors possible. | Big-M indicators under PER grouping, McCormick + PER, Q matrix + PER |
| **MEDIUM** | Missing test for a documented standalone feature, a feature interaction without complex rewrites, or an error case. | Entity-scoped REAL, MINIMIZE with bilinear, three variables |
| **LOW** | Missing edge case unlikely in practice, or a data-shape stress test. | Very large coefficients, 5+ mixed constraints, single-row PER groups |

---

## 1. High-Risk Gaps

These gaps could hide silent correctness bugs. Each involves an optimizer rewrite or linearization interacting with another feature, where the interaction path is entirely unexercised.

### 1.1 PER + Bilinear (McCormick under Grouping)

**Status**: Explicitly documented as untested in `bilinear/done.md`.

**Risk**: McCormick envelope linearization creates auxiliary variables `w` and Big-M constraints (`w <= M*b`, `w >= x - M*(1-b)`, etc.). With PER, these auxiliary constraints must be partitioned by group. If McCormick constraints are generated globally instead of per-group, the feasible region is wrong.

**Covers**: Objectives and constraints with `SUM(b * x) op K PER col`.

```sql
-- Bilinear objective with PER
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, 3.0 AS profit UNION ALL
    SELECT 2, 'A', 1.0 UNION ALL
    SELECT 3, 'B', 5.0 UNION ALL
    SELECT 4, 'B', 2.0
)
SELECT id, grp, b, ROUND(x, 2) AS x
FROM data
DECIDE b IS BOOLEAN, x IS REAL
SUCH THAT x <= 10 AND SUM(b * x) <= 15 PER grp
MAXIMIZE SUM(profit * b * x)
```

### 1.2 Strict `<` / `>` with REAL Variables

**Status**: No test uses strict inequality with continuous variables.

**Risk**: For integer variables, the oracle converts `SUM(x) < 100` to `SUM(x) <= 99`, which is mathematically equivalent. For REAL variables, this is **wrong** — `SUM(x) < 100` is a strict open constraint, not the same as `<= 99`. If PackDB also converts `<` to `<= val-1` internally for REAL, results are silently incorrect. This may also be an oracle bug — the oracle may need fixing before the test can be written.

**Covers**: `test_cons_comparison.py` strict inequality tests, but only for INTEGER.

```sql
-- REAL variables with strict inequality
SELECT id, ROUND(x, 4) AS x FROM data
DECIDE x IS REAL
SUCH THAT x <= 10 AND SUM(x) < 25.5
MAXIMIZE SUM(x)
```

### 1.3 Hard MIN/MAX Constraints with PER

**Status**: Only easy-case PER stripping and PER on objectives are tested. Hard constraint cases (`MAX(expr) >= K`, `MIN(expr) <= K`) with PER have zero coverage.

**Risk**: Hard MIN/MAX constraints create Big-M indicator variables and linking constraints (`z <= x`, etc.). With PER, these must be created per group — one indicator set per distinct PER value. If indicators are created globally, per-group semantics are lost and the solver sees the wrong problem.

**Covers**: The constraint-level indicator+PER interaction (distinct from the objective-level PER tested in `test_per_objective.py`).

```sql
-- Hard MAX constraint with PER
SELECT id, category, cost, profit, x
FROM items
DECIDE x IS BOOLEAN
SUCH THAT MAX(x * cost) >= 50 PER category
MAXIMIZE SUM(x * profit)
```

### 1.4 ABS in Aggregate Constraint with WHEN

**Status**: ABS+WHEN is tested on objectives (`test_abs_objective_with_when`) but not on aggregate constraints.

**Risk**: ABS linearization creates auxiliary variables and unconditional linking constraints (`d >= expr`, `d >= -expr`). When WHEN is present, the auxiliary variable's contribution to the *aggregate* (`SUM(d)`) should include only WHEN-matching rows. If the WHEN filter doesn't propagate to the auxiliary variable's coefficient in the aggregate, wrong rows contribute to the sum.

**Covers**: `SUM(ABS(expr)) op K WHEN condition` — the interaction of ABS auxiliary vars with WHEN filtering on aggregate constraints.

```sql
-- ABS aggregate constraint with WHEN
WITH data AS (
    SELECT 1 AS id, 10.0 AS target, true AS active, 5.0 AS profit UNION ALL
    SELECT 2, 20.0, false, 8.0 UNION ALL
    SELECT 3, 15.0, true, 3.0
)
SELECT id, ROUND(x, 2) AS x, target, active
FROM data
DECIDE x IS REAL
SUCH THAT x <= 30 AND SUM(ABS(x - target)) <= 8 WHEN active
MAXIMIZE SUM(x * profit)
```

### 1.5 PER + ABS in Aggregate Constraint

**Status**: The existing `test_abs_objective_with_per` puts PER on a separate SUM constraint, not on the ABS aggregate itself. ABS auxiliary variables under PER grouping are untested.

**Risk**: Each PER group's aggregate should sum only its own auxiliary variables. If auxiliary variable group assignment is wrong, cross-group contamination occurs.

**Covers**: `SUM(ABS(expr)) op K PER col` — ABS auxiliary variable partitioning under PER.

```sql
-- ABS aggregate constraint with PER
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
    SELECT 2, 'A', 15.0 UNION ALL
    SELECT 3, 'B', 20.0 UNION ALL
    SELECT 4, 'B', 25.0
)
SELECT id, grp, ROUND(x, 2) AS x, target
FROM data
DECIDE x IS REAL
SUCH THAT x <= 50 AND SUM(ABS(x - target)) <= 5 PER grp
MAXIMIZE SUM(x)
```

### 1.6 bilinear + WHEN + PER Triple Interaction

**Status**: Both bilinear+PER (gap 1.1) and the triple combination are untested.

**Risk**: McCormick Big-M generation under PER grouping with WHEN row filtering. All three systems must compose correctly: WHEN filters rows, PER partitions the remaining rows into groups, and McCormick constraints must respect both.

```sql
-- Triple: bilinear + WHEN + PER
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, true AS active, 5.0 AS profit UNION ALL
    SELECT 2, 'A', false, 3.0 UNION ALL
    SELECT 3, 'B', true, 8.0 UNION ALL
    SELECT 4, 'B', true, 2.0
)
SELECT id, grp, b, ROUND(x, 2) AS x
FROM data
DECIDE b IS BOOLEAN, x IS REAL
SUCH THAT x <= 10 AND SUM(b * x) <= 12 WHEN active PER grp
MAXIMIZE SUM(b * x * profit)
```

### 1.7 entity_scope + Bilinear

**Status**: No test combines table-scoped variables with bilinear terms.

**Risk**: Entity deduplication means multiple result rows share one solver variable. McCormick auxiliary variable indexing must use entity-keyed indices, not row indices. A mismatch produces wrong linking constraints.

**Covers**: `DECIDE Table.b IS BOOLEAN, x IS REAL` with `SUM(b * x)` in objective or constraint.

```sql
-- Entity-scoped variable with bilinear
SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN, ROUND(x, 2) AS x
FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
WHERE n.n_regionkey = 0 AND n.n_nationkey <= 5
DECIDE n.keepN IS BOOLEAN, x IS REAL
SUCH THAT x <= 100 AND SUM(keepN * x) <= 500
MAXIMIZE SUM(keepN * x * c_acctbal)
```

### 1.8 COUNT(x) INTEGER with PER

**Status**: `test_count_with_per` in `test_count_rewrite.py` uses BOOLEAN (trivially rewritten to SUM). The INTEGER path (Big-M indicator + PER) has no test.

**Risk**: The Big-M indicator linking constraints (`z <= x`, `x <= M*z`) must be generated per-group or globally depending on the constraint structure. Wrong scoping produces incorrect COUNT semantics per group.

**Covers**: `COUNT(x) op K PER col` where x IS INTEGER.

```sql
-- COUNT INTEGER with PER
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, 10 AS val UNION ALL
    SELECT 2, 'A', 5 UNION ALL
    SELECT 3, 'B', 8 UNION ALL
    SELECT 4, 'B', 3
)
SELECT id, grp, x
FROM data
DECIDE x
SUCH THAT x <= 10 AND COUNT(x) >= 1 PER grp
MAXIMIZE SUM(x * val)
```

### 1.9 QP Objective with PER

**Status**: QP constraints with PER are tested (`test_quadratic_constraints.py::test_per_groups`), but QP *objectives* with PER have zero coverage.

**Risk**: The Q matrix construction must handle PER group auxiliaries. With nested PER objectives (`SUM(SUM(POWER(x-t,2))) PER col`), inner creates per-group Q matrices, outer creates a global sum. A bug here produces wrong optimal values without any error.

```sql
-- QP objective with PER
WITH data AS (
    SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
    SELECT 2, 'A', 15.0 UNION ALL
    SELECT 3, 'B', 20.0 UNION ALL
    SELECT 4, 'B', 25.0
)
SELECT id, grp, ROUND(x, 4) AS x
FROM data
DECIDE x IS REAL
SUCH THAT x >= 0 AND x <= 100 AND SUM(x) >= 5 PER grp
MINIMIZE SUM(POWER(x - target, 2))
```

### 1.10 Unbounded Problem Detection

**Status**: Infeasible problems are tested (4 tests in `test_error_infeasible.py`). Unbounded problems have no test.

**Risk**: When no upper bound constrains a MAXIMIZE objective, the solver returns UNBOUNDED status. PackDB may not handle this status code correctly — could crash, hang, or return garbage values.

```sql
-- Unbounded: no upper bound on x
SELECT id, x FROM data
DECIDE x IS INTEGER
SUCH THAT x >= 1
MAXIMIZE SUM(x)
```

---

## 2. Medium-Risk Gaps

Documented features or error cases without coverage. Bugs here would be caught by users hitting the feature, but could go undetected in CI.

### 2.1 Entity-Scoped IS REAL

**Risk**: `test_entity_scope.py` covers BOOLEAN (tests 1,2,5) and INTEGER (test 3) but never REAL. The DOUBLE readback through `VarIndexer` entity mapping is untested. A type mismatch in the entity-keyed variable path would produce garbage values.

```sql
SELECT n_nationkey, ROUND(budget, 2) AS budget
FROM nation WHERE n_regionkey <= 2
DECIDE nation.budget IS REAL
SUCH THAT budget <= 1000 AND SUM(budget) <= 5000
MAXIMIZE SUM(budget * n_nationkey)
```

### 2.2 Entity-Scoped with Hard MIN/MAX

**Risk**: Only easy case (`MAX <= K`) is tested. Hard cases (`MAX(expr) >= K`, `MIN(expr) <= K`) require Big-M indicators whose indexing must account for entity deduplication.

```sql
SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN
FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
WHERE n.n_regionkey = 0
DECIDE n.keepN IS BOOLEAN
SUCH THAT MAX(keepN * c_acctbal) >= 5000
MAXIMIZE SUM(keepN)
```

### 2.3 Entity-Scoped with `<>`

**Risk**: The NE indicator rewrite must handle entity-keyed variables. The Big-M disjunction constraints reference variable indices that must match the entity-deduplication mapping.

```sql
SELECT c.c_custkey, n.n_nationkey, keepN
FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
WHERE n.n_regionkey = 0 AND n.n_nationkey <= 5
DECIDE n.keepN IS BOOLEAN
SUCH THAT SUM(keepN) <> 3
MAXIMIZE SUM(keepN * c_acctbal)
```

### 2.4 AVG with `<>` Operator

**Risk**: `AVG(x) <> K` desugars to `SUM(x) <> K*N` (RHS scaling). The scaling interacts with the Big-M disjunction rewrite. If RHS scaling happens after `<>` expansion, the disjunction bounds are wrong.

```sql
-- AVG with not-equal
SELECT id, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT AVG(x) <> 0.5
MAXIMIZE SUM(x * profit)
```

### 2.5 Aggregate BETWEEN Without Aggregate-Local WHEN

**Risk**: `SUM(x) BETWEEN 10 AND 50` desugars to `SUM(x) >= 10 AND SUM(x) <= 50`. The only aggregate BETWEEN test is inside `test_aggregate_local_when.py`. The standalone desugaring path is untested — wrong signs or RHS values on aggregate inputs would go unnoticed.

```sql
-- Standalone aggregate BETWEEN
SELECT id, x FROM items
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * weight) BETWEEN 10 AND 50
MAXIMIZE SUM(x * value)
```

### 2.6 Negative Coefficients in Aggregate Constraints

**Risk**: Only negative *objective* coefficients are tested (`test_edge_cases.py`). `SUM(x * col) <= K` where `col` has negative values tests the sign handling during coefficient extraction in `physical_decide.cpp`. A sign error could silently flip constraint direction.

```sql
-- Negative coefficients in aggregate constraint
WITH data AS (
    SELECT 1 AS id, -5.0 AS cost, 10.0 AS val UNION ALL
    SELECT 2, -3.0, 8.0 UNION ALL
    SELECT 3, 7.0, 15.0 UNION ALL
    SELECT 4, -1.0, 5.0
)
SELECT id, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * cost) >= -10
MAXIMIZE SUM(x * val)
```

### 2.7 WHEN on Per-Row Constraint with IS REAL

**Risk**: All per-row WHEN tests use BOOLEAN or INTEGER. REAL variables have different bound semantics (no implicit [0,1] cap), so the WHEN skip-constraint path for continuous variables is untested.

```sql
SELECT id, ROUND(x, 2) AS x, active FROM data
DECIDE x IS REAL
SUCH THAT x <= 10 WHEN active AND SUM(x) <= 30
MAXIMIZE SUM(x * profit)
```

### 2.8 IS REAL with MINIMIZE Objective

**Risk**: All REAL-variable tests use MAXIMIZE. A MINIMIZE LP is a distinct code path for coefficient sign handling.

```sql
SELECT id, ROUND(x, 2) AS x FROM data
DECIDE x IS REAL
SUCH THAT SUM(x) >= 10 AND x <= 100
MINIMIZE SUM(x * cost)
```

### 2.9 Mixed Linear + Quadratic Objective

**Risk**: `SUM(POWER(x - t, 2) + c * x)` — the linear part contributes to `c^T x` and the quadratic part to Q. If they are not composed properly, one component overwrites the other.

```sql
-- Mixed linear + quadratic
SELECT id, ROUND(x, 4) AS x FROM data
DECIDE x IS REAL
SUCH THAT x >= 0 AND x <= 100
MINIMIZE SUM(POWER(x - target, 2) + penalty * x)
```

### 2.10 Quadratic Constraint with WHEN + PER Combined

**Risk**: Tested separately (WHEN in one test, PER in another) but never `SUM(POWER(x - t, 2)) <= K WHEN cond PER col`. The WHEN mask must apply before PER grouping for quadratic constraints just as it does for linear.

```sql
-- Quadratic constraint with WHEN + PER
SELECT id, grp, active, ROUND(x, 4) AS x FROM data
DECIDE x IS REAL
SUCH THAT x >= 0 AND x <= 100
    AND SUM(POWER(x - target, 2)) <= 50 WHEN active PER grp
MAXIMIZE SUM(x)
```

### 2.11 Bilinear in MINIMIZE Objective

**Risk**: All bilinear objective tests use MAXIMIZE. The sign of the Q matrix off-diagonal entries matters for MINIMIZE — if the sign is wrong, MINIMIZE becomes MAXIMIZE for the bilinear component.

```sql
-- Bilinear MINIMIZE
SELECT id, b, ROUND(x, 2) AS x FROM data
DECIDE b IS BOOLEAN, x IS REAL
SUCH THAT x <= 10 AND SUM(b) >= 2
MINIMIZE SUM(b * x * cost)
```

### 2.12 INTEGER + REAL Without BOOLEAN

**Risk**: `DECIDE x IS INTEGER, y IS REAL` is never tested as a pair. MILP variable type flags must be set correctly when no BOOLEAN variables are present.

```sql
SELECT id, x, ROUND(y, 2) AS y FROM data
DECIDE x IS INTEGER, y IS REAL
SUCH THAT x <= 5 AND y <= 10 AND SUM(x + y) <= 20
MAXIMIZE SUM(x * val_a + y * val_b)
```

### 2.13 Three or More Decision Variables

**Risk**: No test uses more than 2 decision variables. Variable indexing in `physical_decide.cpp` could have off-by-one errors only visible with 3+ variables.

```sql
SELECT id, x, y, ROUND(z, 2) AS z FROM data
DECIDE x IS BOOLEAN, y IS INTEGER, z IS REAL
SUCH THAT SUM(x) <= 3 AND y <= 5 AND z <= 10.0
    AND SUM(x * a + y * b + z * c) <= 100
MAXIMIZE SUM(x * val_a + y * val_b + z * val_c)
```

### 2.14 Multiple Variables + PER

**Risk**: Multi-variable coefficient extraction with PER grouping. Each variable's coefficients must be correctly partitioned by group.

```sql
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

### 2.15 WHEN + PER + Multiple Variables Triple

**Risk**: Multi-variable coefficient paths under combined WHEN filtering + PER grouping. Each variable's coefficients in each group must respect the WHEN mask independently.

### 2.16 entity_scope + WHEN + MIN/MAX Triple

**Risk**: Each pair is tested, but all three together are not. Entity dedup + WHEN mask + MIN/MAX indicator generation must compose correctly.

### 2.17 Variable in Objective but Not in Constraints (Unconstrained)

**Risk**: For REAL/INTEGER variables with no bound, the solver pushes the value to infinity, making the problem unbounded. PackDB might not detect this or might return garbage. Related to gap 1.10 but specifically about the unconstrained-variable pattern.

```sql
SELECT id, val, x, y FROM data
DECIDE x IS BOOLEAN, y IS INTEGER
SUCH THAT SUM(x) <= 5
MAXIMIZE SUM(x * val + y)
-- y is unconstrained and unbounded
```

### 2.18 WHEN on Objective Matching Zero Rows

**Risk**: When the objective WHEN filters out ALL rows, every coefficient becomes 0. The constraint WHEN no-match is tested, but the objective path (objective vector construction) is distinct.

```sql
SELECT id, val, flag, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 1
MAXIMIZE SUM(x * val) WHEN flag = 'NONEXISTENT'
```

### 2.19 Feasibility Problem with PER

**Risk**: `DECIDE x IS BOOLEAN SUCH THAT SUM(x) = 1 PER col` without objective. Model builder sets all objective coefficients to zero; constraint generation under PER must still work correctly.

```sql
SELECT id, grp, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) = 1 PER grp
```

### 2.20 MIN/MAX with Aggregate-Local WHEN on Hard Cases

**Risk**: Aggregate-local WHEN test only covers `MAX(x) <= K WHEN` (easy case). No test covers `MAX(x) >= K WHEN` (hard case with indicators + local filter).

### 2.21 MINIMIZE COUNT(x) INTEGER

**Risk**: `MAXIMIZE COUNT(x)` is tested for INTEGER. The indicator variable direction for minimization is untested.

```sql
SELECT id, x FROM data
DECIDE x
SUCH THAT SUM(x) >= 20 AND x <= 10
MINIMIZE COUNT(x)
```

### 2.22 PER Equality Constraint

**Risk**: `SUM(x) = K PER col` generates two-sided bounds per group. Only `<=` and `>=` PER constraints are tested.

```sql
SELECT id, grp, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) = 2 PER grp
MAXIMIZE SUM(x * val)
```

### 2.23 WHEN with IS NULL / IS NOT NULL as Condition

**Risk**: `test_when_null_condition_column` tests NULL *values* in the filtered column, but no test uses `WHEN col IS NULL` or `WHEN col IS NOT NULL` as the WHEN condition syntax. A parse failure would go undetected.

```sql
SELECT id, x, distance FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 3
MAXIMIZE SUM(x * value) WHEN distance IS NOT NULL
```

### 2.24 Correlated Subquery with IS REAL Variable

**Risk**: All correlated subquery tests use BOOLEAN or INTEGER. The decorrelation join produces per-row values; with REAL variables, these could be fractional bounds.

### 2.25 PER on Per-Row Constraint Rejection

**Risk**: Documented restriction: "PER requires an aggregate constraint." No test verifies the error message when `x <= 5 PER col` is attempted. Usability issue — users need clear guidance.

---

## 3. Low-Risk Gaps

Edge cases and unlikely scenarios. Closing these improves robustness but is lower priority.

### 3.1 PER with Zero-Coefficient Groups

Some PER groups where `SUM(x * col)` has all-zero coefficients (e.g., all `col` values are 0 in one group). The constraint becomes trivially satisfied for that group.

### 3.2 Single-Row PER Groups

Data where each PER group has exactly one row. This degenerates PER constraints to per-row bounds. The coefficient vector has a single entry per group.

### 3.3 All-Zero Coefficients in Objective

`MAXIMIZE SUM(x * 0)` — objective is identically zero; the solver returns an arbitrary feasible solution. PackDB may misinterpret the solver status.

### 3.4 Very Large Coefficients (Numeric Stability)

Big-M values derived from large coefficients (e.g., 1e9) weaken LP relaxations and may cause solver numeric warnings or incorrect integrality decisions.

### 3.5 PER Group with 1 Row + AVG

Off-by-one or division issues in `1/n_g` scaling when a PER group has a single row, combined with groups of very different sizes.

### 3.6 NULL in PER Column AND WHEN Condition Simultaneously

Each is tested independently but not composed. Row with NULL PER key that passes WHEN could cause incorrect group count.

### 3.7 5+ Heterogeneous Constraints in Same Query

Maximum in the test suite is about 3 constraints. No stress test for the constraint matrix builder with many mixed constraint types (per-row + aggregate + PER + WHEN + different operators).

### 3.8 JOIN Fan-Out with Row-Scoped Variables

1-to-many JOIN causes duplicate key rows. Each duplicate gets its own variable, which could surprise users expecting entity-level semantics. The entity-scope tests cover this explicitly, but row-scoped behavior on fan-out JOINs is not tested against an oracle.

### 3.9 Uncorrelated Subquery in PER Constraint RHS

`SUM(x) <= (SELECT AVG(col) FROM other_table) PER group`. The scalar RHS must be shared across all PER groups; if execution evaluates it per-group, errors could occur.

### 3.10 Dual-Solver Result Comparison

The oracle picks ONE solver (Gurobi preferred, HiGHS fallback). No test runs both solvers on the same problem and compares. It is unclear whether the full test suite has been validated against HiGHS for non-quadratic tests.

### 3.11 Fractional Solution Verification for IS REAL

No test forces and verifies a genuinely non-integer REAL result (e.g., `SUM(x) = 10` over 3 rows producing x = 3.333...). Existing REAL tests could pass even if the solver silently returned integer-valued solutions.

### 3.12 HiGHS-Specific Error Message Quality

The `_expect_gurobi` decorator catches any `PackDBCliError` containing "quadratic" or "Gurobi", but no dedicated HiGHS-only test verifies the specific error message content for non-convex QP, MIQP, or non-convex bilinear.

### 3.13 Bilinear Bool x Real in Constraint with McCormick Bounds Verification

The bilinear constraint test covers Bool x Bool. No test verifies that McCormick envelope constraints produce correct feasible regions for Bool x Real constraints specifically.

### 3.14 Negative Constant Multiplier in SUM

`SUM(x * (-5)) <= K` or `SUM(-x) <= K`. The symbolic normalizer handles sign, but no explicit test covers a purely negative constant multiplier in an aggregate constraint.

---

## 4. Interaction Matrix Summary

### Well-Tested Pairs

These feature combinations have at least one dedicated test:

| Feature A | Feature B | Test Location |
|-----------|-----------|---------------|
| WHEN | PER | `test_per_clause.py`, `test_per_multi_column.py`, `test_aggregate_local_when.py` |
| WHEN | MIN/MAX | `test_min_max.py`, `test_aggregate_local_when.py` |
| WHEN | COUNT (BOOLEAN) | `test_count_rewrite.py` |
| WHEN | COUNT (INTEGER) | `test_count_integer.py` |
| WHEN | AVG | `test_avg.py`, `test_aggregate_local_when.py` |
| WHEN | ABS (objective) | `test_abs_linearization.py` |
| WHEN | QP | `test_quadratic.py`, `test_quadratic_constraints.py` |
| WHEN | `<>` | `test_cons_comparison.py` (expression-level; aggregate-local is xfail — known bug) |
| WHEN | entity_scope | `test_entity_scope.py`, `test_aggregate_local_when.py` |
| WHEN | bilinear | `test_bilinear.py`, `test_aggregate_local_when.py` |
| PER | MIN/MAX (objectives) | `test_per_objective.py` (6+ tests, all nesting combos) |
| PER | COUNT (BOOLEAN) | `test_count_rewrite.py` |
| PER | AVG | `test_avg.py`, `test_per_objective.py` |
| PER | `<>` | `test_per_clause.py` |
| PER | entity_scope | `test_entity_scope.py` |
| entity_scope | MIN/MAX (easy) | `test_entity_scope.py` |
| entity_scope | COUNT | `test_entity_scope.py` |
| entity_scope | AVG | `test_entity_scope.py` |
| entity_scope | multiple vars | `test_entity_scope.py` |
| entity_scope | QP (constraints) | `test_quadratic_constraints.py` |
| QP | multiple vars | `test_quadratic.py`, `test_quadratic_constraints.py` |
| QP | bilinear | `test_quadratic_constraints.py` |
| BOOLEAN + INTEGER | same query | `test_var_multi.py` |
| BOOLEAN + REAL | same query | `test_var_real.py`, `test_abs_linearization.py` |
| subquery + WHEN | same query | `test_cons_correlated_subquery.py` |
| JOIN + entity_scope | all entity_scope tests | `test_entity_scope.py` |

### Zero-Coverage Pairs

These feature combinations have **no** test:

| Feature A | Feature B | Gap # | Risk |
|-----------|-----------|-------|------|
| PER | bilinear | 1.1 | HIGH |
| PER | ABS (aggregate constraint) | 1.5 | HIGH |
| PER | COUNT (INTEGER) | 1.8 | HIGH |
| PER | QP (objective) | 1.9 | HIGH |
| entity_scope | bilinear | 1.7 | HIGH |
| entity_scope | hard MIN/MAX | 2.2 | MEDIUM |
| entity_scope | `<>` | 2.3 | MEDIUM |
| multiple vars | PER | 2.14 | MEDIUM |

### Triple Interactions

| Features | Tested? | Gap # |
|----------|---------|-------|
| WHEN + PER + MIN/MAX | Yes | -- |
| WHEN + PER + entity_scope | Yes | -- |
| WHEN + PER + AVG | Yes | -- |
| bilinear + WHEN + PER | **No** | 1.6 |
| WHEN + PER + multiple vars | **No** | 2.15 |
| QP + WHEN + multiple vars | **No** | -- |
| entity_scope + WHEN + MIN/MAX | **No** | 2.16 |

---

## 5. Cross-Cutting Observations

1. **PER is the weakest interaction partner**: PER has zero coverage with bilinear, ABS aggregates, COUNT(INTEGER), and QP objectives. All of these involve auxiliary variable / indicator creation, which must be correctly partitioned by group. This is the single biggest gap area.

2. **Bilinear is only tested standalone + WHEN**: No PER, no entity_scope interaction. The bilinear docs explicitly acknowledge PER composition is untested.

3. **IS REAL is under-tested**: No MINIMIZE, no entity-scope, no per-row WHEN. All REAL tests use MAXIMIZE with row-scoped variables. Fractional solution verification is missing.

4. **The oracle's strict inequality handling may be wrong for REAL**: Gap 1.2 may be an oracle bug, not just a missing test. Needs investigation before writing the test.

5. **No test uses 3+ decision variables**: Variable indexing is only tested with 1-2 variables.

6. **Error boundary testing is incomplete**: Unbounded problems and HiGHS-specific error messages lack coverage. Only infeasible errors are tested.

7. **Aggregate-local WHEN + NE is a known bug**: `test_ne_aggregate_local_when_constraint` is xfail. This is a documented defect, not just a missing test.

---

## 6. Known Bugs (xfail)

For completeness, these are known failing tests that represent actual bugs, not missing coverage:

| Test | Status | Issue |
|------|--------|-------|
| `test_ne_aggregate_local_when_constraint` | xfail | NE Big-M expansion doesn't compose with aggregate-local WHEN filters |
