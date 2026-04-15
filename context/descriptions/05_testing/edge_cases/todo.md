# Edge Cases & Data Shapes Test Coverage — Todo

## Missing coverage

### MEDIUM: Variable in objective but not in any constraint (unconstrained)

For REAL/INTEGER variables with no bound or aggregate constraint, the solver pushes the value to infinity, making the problem unbounded. PackDB might not detect this or might return garbage.

See also [error_handling/todo.md](../error_handling/todo.md) for the related unbounded-problem detection gap.

```sql
SELECT id, val, x, y FROM data
DECIDE x IS BOOLEAN, y IS INTEGER
SUCH THAT SUM(x) <= 5
MAXIMIZE SUM(x * val + y)
-- y is unconstrained; solver should detect unbounded
```

### LOW: All-zero coefficients in objective

`MAXIMIZE SUM(x * 0)` — the objective is identically zero; the solver returns an arbitrary feasible solution. PackDB may misinterpret the solver status or the zero objective value.

```sql
WITH data AS (SELECT 1 AS id, 0.0 AS val UNION ALL SELECT 2, 0.0)
SELECT id, val, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 1
MAXIMIZE SUM(x * val)
```

### LOW: Very large coefficients (numeric stability)

Big-M values derived from large coefficients (e.g., 1e9) weaken LP relaxations and can cause solver numeric warnings, incorrect integrality decisions, or outright solver failure.

```sql
WITH data AS (SELECT 1 AS id, 1e9 AS val UNION ALL SELECT 2, 1e9)
SELECT id, val, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) <> 1 AND SUM(x * val) <= 2e9
MAXIMIZE SUM(x * val)
-- <> uses Big-M internally; with 1e9 coefficients Big-M is enormous
```

### LOW: Query with 5+ heterogeneous constraints

Maximum in the test suite is about 3 constraints. No stress test for the constraint matrix builder with many mixed constraint types (per-row + aggregate + PER + WHEN + different operators in the same query). Indexing errors in the constraint matrix could show up only under complex compositions.

```sql
SELECT id, qty, price, x FROM data
DECIDE x IS BOOLEAN
SUCH THAT
    x <= 1
    AND SUM(x * qty) <= 100
    AND SUM(x) >= 5 WHEN flag = 'R'
    AND SUM(x) <= 3 PER category
    AND SUM(x) <> 10
    AND SUM(x * price) BETWEEN 100 AND 500
MAXIMIZE SUM(x * price)
```

### LOW: JOIN fan-out with row-scoped variables

JOINs are tested (`test_sql_joins.py`), but no test specifically verifies behavior when a 1-to-many JOIN produces duplicate key rows with *row-scoped* variables. Each duplicate gets its own solver variable, which could surprise users expecting entity-level semantics. The entity-scope tests cover this explicitly; the row-scoped variant on a fan-out JOIN is not tested against an oracle.

```sql
-- Fan-out JOIN (orders × lineitems is 1-to-many)
SELECT o.o_orderkey, l.l_linenumber, l.l_quantity, x
FROM orders o JOIN lineitem l ON o.o_orderkey = l.l_orderkey
WHERE o.o_orderkey <= 10
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_quantity) <= 50
MAXIMIZE SUM(x * l_extendedprice)
```

### LOW: Dual-solver result comparison (Gurobi vs HiGHS)

The oracle solver factory picks ONE solver per session. There is no test that runs both solvers on the same problem and compares results. It is unclear whether the full test suite has been validated against HiGHS for non-quadratic tests, since Gurobi is preferred when available.

```python
# Pseudo: dual-solver comparison fixture
@pytest.mark.dual_solver
def test_gurobi_highs_agree_on_objective(packdb_conn, sql_query):
    gurobi_result = run_with_solver(packdb_conn, sql_query, solver="gurobi")
    highs_result = run_with_solver(packdb_conn, sql_query, solver="highs")
    assert abs(gurobi_result.objective - highs_result.objective) < 1e-4
```

### LOW: Constraint with LHS and RHS both containing decision variables (aggregate)

Per-row multi-variable constraints exist (`x <= y` tested as an error in `test_error_binder.py`). There is no test for an *aggregate* constraint where both sides have decision variables (`SUM(x * val) <= SUM(y * val)`). Should be rejected; no test verifies the specific pattern.

### LOW: Objective with many terms (10+ columns)

TPC-H queries use 2-3 columns in expressions. No test explicitly constructs a very wide linear combination to verify the symbolic normalizer and expression evaluator handle it correctly.

### UNCLEAR: Infeasible quadratic constraint error message

Infeasible linear is tested in `test_error_infeasible.py`; an infeasible quadratic constraint exists in `test_quadratic_constraints.py` but the error matching could be tighter.
