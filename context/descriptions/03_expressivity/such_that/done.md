# SUCH THAT Clause — Implemented Features

The `SUCH THAT` clause specifies the **constraints** of a DECIQL query. The solver only accepts variable assignments that satisfy all constraints.

---

## Syntax

```sql
SUCH THAT
    constraint_expression
    [AND constraint_expression2 ...]
```

Multiple constraints are separated by `AND`. Each constraint evaluates to a boolean.

---

## Constraint Types

### Simple Comparison Constraints

Any comparison involving at least one decision variable (or aggregate over one).

**Supported operators**: `=`, `<`, `<=`, `>`, `>=`, `<>`

`<>` (not-equal) is supported on both per-row and aggregate constraints using Big-M disjunction with an auxiliary binary indicator variable. See [../sql_functions/done.md](../sql_functions/done.md) for linearization details.

```sql
SUCH THAT
    x <= 1                      -- per-row: each x is at most 1
    SUM(x * weight) <= 50       -- aggregate: total weight under budget
    SUM(x) >= 10                -- at least 10 items selected
    SUM(x) <> 5                 -- not-equal via Big-M disjunction
```

**Strict `<` / `>` require an integer-valued LHS.** Internally `LHS < K` is rewritten to `LHS <= ceil(K) - 1`, which is only equivalent to the strict inequality when the LHS can take integer values — i.e., every referenced DECIDE variable is `IS INTEGER` or `IS BOOLEAN` and every coefficient is integral. Bilinear products `b * n` between a Boolean and an Integer (or Integer × Integer) count as integer-valued: the McCormick auxiliary for `b * n` is declared `INTEGER` in `decide_optimizer.cpp:RewriteBilinear`, preserving integer-valuedness through linearization. If any term makes the LHS continuous (a `IS REAL` variable, a fractional coefficient, or a bilinear product involving a `IS REAL` factor), PackDB raises `InvalidInputException` at model-build time; use `<=` / `>=` instead. Enforced in `src/packdb/utility/ilp_model_builder.cpp` (`IsEvalConstraintLhsIntegerValued` + `ApplyComparisonSense`, plus the parallel check in `BuildQuadraticConstraint`).

**`<>` (not-equal) also requires an integer-valued LHS.** `LHS <> K` is rewritten into the Big-M disjunction `LHS <= K-1  OR  LHS >= K+1`, which only spans the full feasible region when `LHS` can take integer values. On a REAL variable or with a non-integer coefficient the band `(K-1, K+1)` is continuous and wrongly excluded. PackDB raises `InvalidInputException` in the same cases as strict `<` / `>`; use a reformulation such as adding an ε-band with `<=` / `>=` if the application can tolerate a small gap. Enforced at the NE expansion site in `src/execution/operator/decide/physical_decide.cpp` (covers both per-row and deferred aggregate NE paths).

### BETWEEN

`expr BETWEEN a AND b` desugars to `expr >= a AND expr <= b`. Both bounds become separate constraints.

```sql
SUCH THAT
    SUM(keep) BETWEEN 200 AND 400      -- between 200 and 400 items selected
```

### IN

`column IN (val1, val2, ...)` constrains a **table column** value to be in the provided set. Also supported on **decision variables**.

```sql
SUCH THAT
    category IN ('A', 'B', 'C')    -- row-level filter on a table column
    x IN (0, 1, 3)                 -- decision variable domain restriction
```

**Decision variable IN**: `x IN (v1, ..., vK)` is rewritten at bind time into K auxiliary binary indicator variables. See [../sql_functions/done.md](../sql_functions/done.md) for the full rewrite details, optimizations, and complexity analysis.

---

## AND Separator

Constraints are joined by `AND`. Each `AND`-separated expression is a distinct constraint in the optimization model.

> **Important**: `AND` inside a `WHEN` condition has different meaning — it is part of the condition, not a constraint separator. Use parentheses: `SUM(x * w) <= 50 WHEN (cat = 'A' AND status = 'active')`.

---

## Aggregate vs. Per-Row Constraints

**Aggregate constraints** use `SUM(...)` over the relation (or filtered subset). These compile to a single linear inequality in the ILP.

```sql
SUM(x * weight) <= 50                          -- one constraint over all rows
SUM(x * weight) <= 20 WHEN category = 'A'      -- one constraint over category-A rows
SUM(x * weight) WHEN a + SUM(x * hours) WHEN b <= 50  -- independent filters per aggregate term
```

**Per-row constraints** do not use an aggregate. The system generates one constraint per input row.

```sql
x <= 1                          -- one constraint generated per row
```

---

## Linearity and Quadratic Support

Expressions involving decision variables must be linear, bilinear, or quadratic:

| Expression | Status |
|---|---|
| `x * 5` — variable times literal | OK |
| `x * column` — variable times table column | OK |
| `SUM(x * column)` | OK |
| `x + y` | OK |
| `x * y` — two different variables (bilinear) | OK (Gurobi only for non-Boolean) |
| `POWER(x - target, 2)` — quadratic | OK (Gurobi only) |
| `(x - target) ** 2` — quadratic | OK (Gurobi only) |
| `(x - target) * (x - target)` — self-product | OK (Gurobi only) |
| `x * x * x` — triple product | **ERROR: not supported** |

### Quadratic Constraints (QCQP)

`POWER(linear_expr, 2)` is supported in constraints, enabling quadratic constraint programming. The inner expression must be linear in decision variables.

```sql
SUCH THAT
    POWER(x - target, 2) <= 9              -- per-row: each row's squared deviation bounded
    SUM(POWER(x - target, 2)) <= 1000      -- aggregate: total squared deviation budget
    SUM(POWER(x - target, 2)) <= 50 PER grp  -- grouped: budget per group
    SUM(POWER(x - target, 2)) <= 10 WHEN active = 1  -- conditional: only active rows
```

**Solver support**: Gurobi only (via `GRBaddqconstr`). HiGHS rejects with a clear error message.

**Syntax forms** (all equivalent): `POWER(expr, 2)`, `POW(expr, 2)`, `expr ** 2`, `(expr) * (expr)` (self-product). Negated (`-POWER(expr, 2)`) and scaled (`K * POWER(expr, 2)`) forms are also supported.

**Composability**: Quadratic constraints compose with WHEN, PER, linear constraints, bilinear terms, and quadratic objectives (QCQP). Multiple quadratic constraints per query are supported.

See [problem_types/done.md](../problem_types/done.md) for the full QCQP problem class description.

---

## Subqueries in Constraints

### Uncorrelated Scalar Subqueries

A subquery that returns a single value and does not reference the outer query's table is allowed. Evaluated once and treated as a constant.

```sql
SUCH THAT
    x <= (SELECT avg(budget) FROM Depts)
```

### Correlated Scalar Subqueries

Correlated subqueries that reference outer table columns are supported. They are delegated to DuckDB's standard `ExpressionBinder`, which decorrelates them into joins via `PlanSubqueries`, producing per-row values.

**Per-row constraints**: The correlated subquery naturally becomes a per-row bound — each row gets its own RHS value from the join.

```sql
SUCH THAT
    x <= (SELECT budget FROM Depts WHERE Depts.id = items.dept_id)
```

**Aggregate constraints**: The subquery RHS must evaluate to the **same scalar for all rows**. If the RHS varies per row, an error is thrown at execution time. This is because an aggregate constraint (`SUM(...)`) compiles to a single linear inequality, which requires a single RHS value.

```sql
-- Works: all items in the same region get the same budget
SUCH THAT
    SUM(x * price) <= (SELECT max(budget) FROM Depts WHERE Depts.region = items.region)

-- Error: if items span multiple regions, the RHS varies per row
-- "Aggregate constraint (SUM/AVG) requires a scalar right-hand side,
--  but the RHS evaluates to different values per row"
```

**Restriction**: Subqueries cannot reference DECIDE variables (checked at bind time via `ExpressionContainsDecideVariable`).

---

## WHEN (Conditional Constraints)

Constraints can be made conditional using the postfix `WHEN` keyword. See [when/done.md](../when/done.md) for full details.

```sql
SUCH THAT
    SUM(x * weight) <= 50 WHEN category = 'electronics' AND
    SUM(x * weight) <= 30 WHEN category = 'clothing'
```

Aggregate-local `WHEN` filters only one aggregate term:

```sql
SUCH THAT
    SUM(x * weight) WHEN category_a + SUM(x * volume) WHEN category_b <= 50
```

---

## Examples

```sql
-- Knapsack with two resource limits
SUCH THAT
    SUM(x * weight) <= 50 AND
    SUM(x * volume) <= 30

-- Conditional weight limits per category
SUCH THAT
    SUM(x * weight) <= 50 WHEN category = 'electronics' AND
    SUM(x * weight) <= 30 WHEN category = 'clothing' AND
    x <= 1
```

---

## Code Pointers

- **Constraint binder**: `src/planner/expression_binder/decide_constraints_binder.cpp`
  - `BindComparison()` — handles `=`, `<`, `<=`, `>`, `>=`, `<>` (all operators on both per-row and aggregate)
  - `BindBetween()` — desugars to two comparison constraints
  - `BindOperator()` — handles IN clause
  - `BindWhenConstraint()` — handles WHEN modifier
  - Nested `WHEN` dispatch through `DecideBinder::BindLocalWhenAggregate()` — handles aggregate-local WHEN filters
  - `BindPerConstraint()` — handles PER modifier
  - Validates that only SUM, COUNT, AVG, MIN, and MAX are used as aggregate functions

- **Subquery handling**: `src/planner/expression_binder/decide_binder.cpp`
  - `DecideBinder::BindExpression()` — validates scalar-only, no DECIDE variable references, then delegates to `ExpressionBinder::BindExpression` for both uncorrelated and correlated subqueries
  - Correlated subquery RHS validation at execution: `src/packdb/utility/ilp_model_builder.cpp`, `SolverModel::Build()` — checks that aggregate constraint RHS is scalar

- **Execution** (constraint matrix construction): `src/execution/operator/decide/physical_decide.cpp`
