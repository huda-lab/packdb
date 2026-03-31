# Problem Types — Implemented Features

PackDB can express several classes of mathematical optimization problems. The problem class is determined automatically by the combination of variable types declared in `DECIDE` and the form of the objective expression. This page catalogs the supported problem classes from a mathematical optimization perspective — for syntax details, see the per-keyword done.md files.

---

## Supported Problem Classes

### LP (Linear Programming)

All variables declared `IS REAL`, with a linear objective and linear constraints.

Standard form: minimize c^T x, subject to Ax <= b, x >= 0

```sql
SELECT id, ROUND(x, 2) FROM data
DECIDE x IS REAL
SUCH THAT x <= 100 AND SUM(x) >= 500
MINIMIZE SUM(x * cost)
```

Supported by both Gurobi and HiGHS.

### ILP (Integer Linear Programming)

All variables declared `IS INTEGER` and/or `IS BOOLEAN`, with a linear objective and linear constraints. This is the **default** problem class — `DECIDE x` without a type annotation defaults to `IS INTEGER`.

Standard form: minimize c^T x, subject to Ax <= b, x in Z^n_+

```sql
SELECT * FROM items
DECIDE keep IS BOOLEAN
SUCH THAT SUM(keep * weight) <= 50
MAXIMIZE SUM(keep * value)
```

Supported by both Gurobi and HiGHS.

### MILP (Mixed-Integer Linear Programming)

Mix of `IS REAL` + `IS INTEGER`/`IS BOOLEAN` variables, with a linear objective and linear constraints. Arises when a query needs both selection (BOOLEAN) and value-assignment (REAL).

```sql
SELECT * FROM employees
DECIDE selected IS BOOLEAN, new_salary IS REAL
SUCH THAT selected <= 1 AND new_salary <= max_salary AND SUM(new_salary) <= budget
MAXIMIZE SUM(selected * performance)
```

Supported by both Gurobi and HiGHS.

### QP (Quadratic Programming)

All variables declared `IS REAL` (continuous), with a **convex quadratic objective** and linear constraints.

Standard form: minimize (1/2) x^T Q x + c^T x, subject to Ax <= b, x >= 0

```sql
SELECT id, ROUND(x, 2) FROM data
DECIDE x IS REAL
SUCH THAT x >= 0 AND x <= 100 AND SUM(x) >= 500
MINIMIZE SUM(POWER(x - target, 2))
```

Supported by both Gurobi and HiGHS.

### MIQP (Mixed-Integer Quadratic Programming)

Mix of `IS REAL` + `IS INTEGER`/`IS BOOLEAN` variables, with a convex quadratic objective and linear constraints. Arises when combining selection (BOOLEAN) with least-squares fitting (REAL).

**Gurobi only** — HiGHS rejects MIQP with an error directing the user to either install Gurobi or use `IS REAL` variables.

```sql
SELECT * FROM measurements
DECIDE keep IS BOOLEAN, repaired IS REAL
SUCH THAT repaired >= 0 AND repaired <= 100 AND SUM(keep) >= 10
MINIMIZE SUM(POWER(repaired - measured, 2))
```

### Feasibility Problems (No Objective)

> **Not yet implemented.** The grammar currently requires `MAXIMIZE` or `MINIMIZE`. This section describes the planned behavior — see `problem_types/todo.md` for status.

Any combination of variable types, with constraints but **no** `MAXIMIZE`/`MINIMIZE` clause. The solver finds any feasible assignment satisfying all constraints.

```sql
SELECT * FROM shifts
DECIDE assigned IS BOOLEAN
SUCH THAT SUM(assigned) >= 3 PER day AND SUM(assigned) <= 5 PER employee
```

Both Gurobi and HiGHS support feasibility problems natively.

---

## How Problem Class Is Determined

The user does not declare a problem class. PackDB infers it from the variable types and objective form:

| Variable Types | Objective | Problem Class |
|---|---|---|
| All BOOLEAN/INTEGER | Linear or none | ILP |
| All REAL | Linear or none | LP |
| Mix of REAL + INTEGER/BOOLEAN | Linear or none | MILP |
| All REAL | Quadratic (`MINIMIZE SUM(POWER)`) | QP |
| Mix of REAL + INTEGER/BOOLEAN | Quadratic | MIQP (Gurobi only) |
| Any | None (feasibility) | Feasibility |

The model builder sets `is_integer`, `is_binary`, and `has_quadratic_objective` flags accordingly, and the solver backend handles the appropriate formulation.

---

## Solver Support Matrix

| Problem Class | Gurobi | HiGHS |
|---|---|---|
| LP | Yes | Yes |
| ILP | Yes | Yes |
| MILP | Yes | Yes |
| QP | Yes | Yes |
| MIQP | Yes | **No** (error) |
| Feasibility | Yes | Yes |

---

## Key Structural Properties

### Constraints Are Always Linear

All `SUCH THAT` constraints are linear by design. Products of two decision variables (`x * y`) are rejected at bind time. This means the feasible region is always a convex polytope (or the integer points within one), regardless of problem class.

### Quadratic Objectives Are Syntax-Enforced Convex

The only supported quadratic form is `MINIMIZE SUM(POWER(linear_expr, 2))`. This guarantees Q = A^T A (positive semidefinite) without runtime checks. The following are rejected:

- `MAXIMIZE SUM(POWER(x, 2))` — maximizing a sum of squares is non-convex
- `MINIMIZE SUM(POWER(x, 3))` — only exponent 2 is supported
- `MINIMIZE SUM(x * y)` — product of different DECIDE variables is not allowed

**Syntax forms** (all equivalent):

```sql
MINIMIZE SUM(POWER(x - target, 2))         -- POWER function
MINIMIZE SUM((x - target) ** 2)             -- ** operator
MINIMIZE SUM((x - target) * (x - target))   -- explicit self-multiplication
```

**Mathematical formulation**: The objective `MINIMIZE SUM(POWER(a1*x + a2*y + c, 2))` is expanded into the standard QP form `(1/2) x^T Q x + c^T x` where:
- Q is built from the outer product of the inner linear expression's coefficients (Q[i,j] = 2*a_i*a_j summed over rows; factor of 2 on all entries due to the (1/2) x^T Q x convention)
- Linear terms arise from constant parts of the inner expression (cross terms: 2*c*a_i)
- The Q matrix is stored in COO (Coordinate) format in `SolverModel`, then converted to CSC for HiGHS

### All Variables Are Non-Negative

All variable types have a default lower bound of 0:
- BOOLEAN: [0, 1]
- INTEGER: [0, 1e30]
- REAL: [0, 1e30]

PackDB cannot currently express problems requiring negative variable values (see [todo.md](todo.md)).

### Big-M Linearization Is Transparent

Several constructs (`<>`, `IN` on decision variables, `COUNT` on INTEGER, hard `MIN`/`MAX` cases) use Big-M linearization internally. From the problem classification perspective, these remain linear — they add auxiliary binary variables and constraints but do not change the user-visible problem class (though an LP with Big-M becomes a MILP internally due to the auxiliary binaries).

---

## Cross-References

- Variable type declarations: [decide/done.md](../decide/done.md)
- Objective forms (linear): [maximize_minimize/done.md](../maximize_minimize/done.md)
- Constraint forms: [such_that/done.md](../such_that/done.md)
- SQL functions and linearization: [sql_functions/done.md](../sql_functions/done.md)
- Solver backends and dispatch: [01_pipeline/03d_solver_backends.md](../../01_pipeline/03d_solver_backends.md)
- Model building (variable type -> solver flags): [01_pipeline/03c_model_building.md](../../01_pipeline/03c_model_building.md)

---

## Code Pointers

- **Variable type -> solver flags**: `src/packdb/utility/ilp_model_builder.cpp`
  - DOUBLE/FLOAT -> `is_integer=false`, BOOLEAN -> `is_binary=true`, INTEGER -> `is_integer=true`
  - These flags determine whether the solver treats the problem as LP, ILP, or MILP

- **Quadratic objective detection**: `src/execution/operator/decide/physical_decide.cpp`
  - Detects `POWER(expr, 2)` and `(expr) * (expr)` patterns in bound expressions
  - Enforces MINIMIZE-only for quadratic objectives
  - Extracts inner linear expression terms into `Objective::squared_terms`

- **Q matrix construction**: `src/packdb/utility/ilp_model_builder.cpp`
  - Builds Q from outer products of per-row inner expression coefficients (guarantees PSD)
  - Handles constant-term cross-contributions to linear objective

- **Gurobi QP**: `src/packdb/gurobi/gurobi_solver.cpp` — calls `GRBaddqpterms` for Q matrix; handles both QP and MIQP natively

- **HiGHS QP**: `src/packdb/naive/deterministic_naive.cpp` — calls `passHessian` with COO->CSC conversion; rejects MIQP with error

- **MIQP rejection (HiGHS)**: `src/packdb/naive/deterministic_naive.cpp`
  - Checks if any variable is integer/boolean when `has_quadratic_obj` is true; throws `InvalidInputException`

- **Solver dispatch**: `src/packdb/utility/ilp_solver.cpp`
  - `SolverModel::Build()` constructs the formulation; `SolveModel()` dispatches to Gurobi (if available) or HiGHS

- **Solver input (Q matrix storage)**: `src/include/duckdb/packdb/solver_input.hpp`
  - `quadratic_inner_coefficients`, `quadratic_inner_variable_indices`, `has_quadratic_objective`

- **SUM argument validation (QP syntax)**: `src/planner/expression_binder/decide_binder.cpp`
  - `ValidateSumArgumentInternal` accepts `POWER(linear_expr, 2)`, `POW(linear_expr, 2)`, and `(expr) * (expr)` where both sides are identical
  - Rejects `POWER(expr, N)` for N != 2, products of different DECIDE variables, non-constant exponents
