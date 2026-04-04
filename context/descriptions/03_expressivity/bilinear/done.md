# Bilinear Terms (`x * y`) — Implemented Features

Products of two different DECIDE variables are supported in both objectives and constraints. The behavior depends on whether one of the variables is Boolean.

---

## Two Categories

### 1. Boolean x Anything (McCormick Linearization)

When one factor is declared `IS BOOLEAN`, the product `b * x` is exactly linearized using McCormick envelopes. This produces an equivalent MILP reformulation — no relaxation, exact for binary variables. Works with both Gurobi and HiGHS.

**Requires**: A finite upper bound on the non-Boolean variable (`x <= K`).

**Bool x Bool** uses simpler AND-linearization (no Big-M needed). The auxiliary `w` satisfies `w = min(b1, b2)`, i.e., logical AND.

**Bool x Non-Bool** uses McCormick envelopes with Big-M:
- Structural constraint `w <= x` (generated at optimizer time)
- Big-M constraints `w <= U*b` and `w >= x - U*(1-b)` (generated at execution time, where U = upper bound on x)

### 2. General Non-Convex (Q Matrix)

When neither factor is Boolean (`Real*Real`, `Int*Int`, `Int*Real`), the product produces off-diagonal entries in the Q matrix. This is always indefinite (non-convex).

- **Objectives**: Gurobi only (via `NonConvex=2`). HiGHS rejects with a clear error.
- **Constraints**: Gurobi only (via `GRBaddqconstr`). HiGHS rejects with a clear error.

---

## Syntax

Bilinear terms appear naturally in `SUM()` expressions:

```sql
-- Boolean x Real objective (McCormick, both solvers)
MAXIMIZE SUM(b * x)

-- With data coefficient
MAXIMIZE SUM(profit * b * x)

-- Boolean x Boolean constraint (AND-linearization, both solvers)
SUCH THAT SUM(b1 * b2) <= 5

-- Non-convex objective (Gurobi only)
MINIMIZE SUM(x * y)

-- Mixed linear + bilinear
MAXIMIZE SUM(cost + b * x)
```

### Composability

Bilinear terms compose with existing features:
- **WHEN**: `MAXIMIZE SUM(b * x) WHEN category = 'A'`
- **PER**: Not yet tested but should work via the standard PER machinery
- **Mixed with POWER**: `MINIMIZE SUM(POWER(x - target, 2) + b * x)` (bilinear + quadratic in same objective)

---

## Implementation Architecture

### Pipeline Flow

1. **Binder** (`decide_binder.cpp`): Relaxed validation to allow `decide_count == 2` products when `allow_quadratic` or `allow_bilinear` is true. Triple products (`a * b * c`) rejected. `allow_bilinear` parameter added for constraints (separate from `allow_quadratic` to prevent POWER in constraints).

2. **Symbolic** (`decide_symbolic.cpp`): `SumInnerContainsBilinear()` detects bilinear content and skips symbolic expansion (which would destroy the `x * y` structure). Handles scaled/negated forms.

3. **Optimizer** (`decide_optimizer.cpp`): `RewriteBilinear()` pass runs after `RewriteAbs`, before `RewriteMinMax`. Walks both objective and constraint expressions:
   - Detects `*` nodes where both children reference different decide variables
   - Skips identical expressions (existing QP path)
   - For Bool x Bool: AND-linearization with 3 structural constraints, BOOLEAN auxiliary
   - For Bool x Non-Bool: structural constraint `w <= x` + `BilinearLink` for execution-time Big-M
   - For Non-Boolean x Non-Boolean: left in place for Q matrix path
   - Uses `is_boolean_var` vector (not `return_type`) to detect boolean status

4. **Physical Operator** (`physical_decide.cpp`):
   - `ExtractLinearAndBilinearTerms()`: separates linear and bilinear terms in objectives
   - `ExtractConstraintTerms()`: same for constraints
   - McCormick Big-M generation: uses `BilinearLink` metadata + `ExtractVariableBounds` to generate `w <= U*b` and `w >= x - U*(1-b)` constraints
   - Evaluates bilinear coefficients per-row, applies WHEN mask

5. **Model Builder** (`ilp_model_builder.cpp`):
   - Bilinear off-diagonal Q entries: `q_map[{max(flat_a,flat_b), min(flat_a,flat_b)}] += coeff`
   - Quadratic constraints: `QuadraticConstraint` struct with separate linear and quadratic parts
   - Skips `INVALID_INDEX` variable entries (constant data terms in mixed objectives)

6. **Solvers**:
   - Gurobi: `GRBaddqpterms` for Q matrix (existing), `GRBaddqconstr` for quadratic constraints (new)
   - HiGHS: rejects non-convex Q and quadratic constraints with clear errors

### Key Data Structures

- `LogicalDecide::BilinearLink` — `{aux_idx, bool_var_idx, other_var_idx}` for execution-time Big-M
- `LogicalDecide::is_boolean_var` — per-variable boolean flag (since `return_type` is INTEGER for all non-REAL vars)
- `SolverInput::BilinearObjectiveTerm` — `{var_a, var_b, row_coefficients}`
- `SolverInput::EvaluatedConstraint::BilinearTerm` — same structure for constraints
- `SolverModel::QuadraticConstraint` — linear + quadratic parts for `GRBaddqconstr`

---

## Code Pointers

- **Binder validation**: `src/planner/expression_binder/decide_binder.cpp` — `ValidateSumArgumentInternal()`, `allow_bilinear` parameter
- **Constraint binder**: `src/planner/expression_binder/decide_constraints_binder.cpp` — passes `allow_bilinear=true`
- **Symbolic bilinear detection**: `src/packdb/symbolic/decide_symbolic.cpp` — `SumInnerContainsBilinear()`, `SumInnerContainsBilinearCore()`
- **Optimizer rewrite**: `src/optimizer/decide/decide_optimizer.cpp` — `RewriteBilinear()`, `FindAndReplaceBilinear()`
- **Boolean type tracking**: `src/include/duckdb/planner/operator/logical_decide.hpp` — `is_boolean_var`
- **Bilinear link struct**: `src/include/duckdb/planner/operator/logical_decide.hpp` — `BilinearLink`
- **Physical execution**: `src/execution/operator/decide/physical_decide.cpp` — `ExtractLinearAndBilinearTerms()`, `ExtractConstraintTerms()`, McCormick Big-M generation
- **Model builder**: `src/packdb/utility/ilp_model_builder.cpp` — Q matrix off-diagonal entries, `QuadraticConstraint` building
- **Gurobi quadratic constraints**: `src/packdb/gurobi/gurobi_solver.cpp` — `GRBaddqconstr` loop
- **HiGHS rejection**: `src/packdb/naive/deterministic_naive.cpp` — quadratic constraint check
- **Serialization**: `src/storage/serialization/serialize_logical_operator.cpp` — properties 225-228

---

## Error Messages

- `"Triple or higher-order products of DECIDE variables are not supported"` — three or more vars in a single product
- `"Bilinear term requires a finite upper bound on variable 'x'"` — McCormick needs `x <= K`
- `"Non-convex quadratic objectives require Gurobi"` — Real*Real or Int*Int bilinear on HiGHS
- `"Quadratic/bilinear constraints require Gurobi"` — non-Boolean bilinear in constraints on HiGHS

---

## Tests

`test/decide/tests/test_bilinear.py` — 17 tests covering:
- Bool x Bool objectives (AND-linearization)
- Bool x Real, Bool x Int objectives (McCormick)
- Data coefficient scaling (`profit * b * x`)
- Non-convex objectives (Real*Real, Int*Int, Int*Real — Gurobi only)
- Mixed linear + bilinear objectives
- Bilinear with WHEN filter
- Bool bilinear constraints
- Error cases (triple product, missing bounds, HiGHS rejection)
- Backward compatibility (POWER, linear, identical multiplication)
