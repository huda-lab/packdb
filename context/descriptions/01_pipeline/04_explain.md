# EXPLAIN Support for DECIDE

## Overview

The DECIDE operator supports all three forms of DuckDB's `EXPLAIN` mechanism:

- **`EXPLAIN`** — displays the query plan (logical or physical) with DECIDE node details
- **`EXPLAIN ANALYZE`** — executes the query and includes timing and row count profiling
- **`EXPLAIN (FORMAT JSON)`** — outputs the plan in JSON format

Both the `LogicalDecide` and `PhysicalDecide` operators override `GetName()` and `ParamsToString()` to produce structured output that DuckDB's plan renderer consumes.

---

## EXPLAIN Output Structure

The DECIDE node displays three sections:

1. **Variables** — user-declared decision variables (auxiliary variables are excluded)
2. **Objective** — `MAXIMIZE` or `MINIMIZE` followed by the objective expression
3. **Constraints** — individual constraints, each on its own line, with WHEN/PER suffixes

Example output for a basic knapsack query:

```
┌───────────────────────────┐
│           DECIDE          │
│    ────────────────────   │
│        Variables: x       │
│                           │
│         Objective:        │
│  MAXIMIZE sum((CAST(x AS  │
│      DECIMAL(18,0)) *     │
│      l_extendedprice))    │
│                           │
│        Constraints:       │
│ (x >= CAST(0 AS INTEGER)) │
│ (x <= CAST(1 AS INTEGER)) │
│    (CAST(sum((CAST(x AS   │
│      DECIMAL(18,0)) *     │
│  l_quantity)) AS DOUBLE) <│
│       = 100.0) WHEN       │
│  (l_returnflag = CAST('R' │
│        AS VARCHAR))       │
│                           │
│        ~12035 Rows        │
└─────────────┬─────────────┘
```

---

## AND-Tree Splitting

DuckDB binds multiple `SUCH THAT` constraints into a single `BoundConjunctionExpression` (AND-tree). Printing the root expression as one string would produce an unreadable single line. Instead, a recursive `CollectConstraintStrings` function traverses the tree:

1. **AND nodes** — recurse into each child
2. **WHEN wrappers** (`alias == WHEN_CONSTRAINT_TAG`) — extract the condition from `children[1]`, recurse into `children[0]`, and append ` WHEN <condition>` to each leaf
3. **PER wrappers** (`alias == PER_CONSTRAINT_TAG`) — extract column names from `children[1..N]`, recurse into `children[0]`, and append ` PER <columns>` to each leaf
4. **Leaf nodes** (comparisons, aggregates) — emit `expr.GetName()`

This produces one line per constraint with WHEN/PER suffixes, e.g.:

```
(sum(x * weight) <= 50.0) WHEN (returnflag = 'R') PER department
```

---

## EXPLAIN ANALYZE

`EXPLAIN ANALYZE` executes the query and reports per-operator timing and row counts. No manual instrumentation is needed — DuckDB's `PipelineExecutor` automatically measures time spent in each operator's `Sink`, `Finalize`, and `Source` phases.

Row counts in `EXPLAIN ANALYZE` show **actual** counts, not estimates. Since DECIDE is an annotation operator (it assigns values to every input row without filtering), the row count is the same across all nodes in the plan.

For small datasets, per-operator timing may display as `(0.00s)` due to two-decimal-place rounding. The `Total Time` at the top of the output is the reliable measurement.

---

## EXPLAIN (FORMAT JSON)

JSON output uses the same `GetName()` and `ParamsToString()` methods. It does **not** use `Serialize`/`Deserialize` — those are for prepared statement caching and plan storage, not EXPLAIN rendering.

---

## Cardinality Estimates

DECIDE does not override `EstimateCardinality`. The default behavior propagates the child's estimate upward, which is correct: DECIDE emits every input row (it assigns decision variable values, it does not filter). The `~N Rows` estimate in `EXPLAIN` and the `N Rows` count in `EXPLAIN ANALYZE` will match the scan's cardinality.

---

## Code Pointers

### Logical Layer

- **`GetName()` / `ParamsToString()`**: `src/planner/operator/logical_decide.cpp` (lines 47–128)
- **`CollectConstraintStrings()`**: `src/planner/operator/logical_decide.cpp` (lines 51–89)
- **Declaration**: `src/include/duckdb/planner/operator/logical_decide.hpp` (lines 77–78, 90)

### Physical Layer

- **`GetName()` / `ParamsToString()`**: `src/execution/operator/decide/physical_decide.cpp` (lines 245–320)
- **`CollectConstraintStringsPhysical()`**: `src/execution/operator/decide/physical_decide.cpp` (lines 249–282)
- **Declaration**: `src/include/duckdb/execution/operator/decide/physical_decide.hpp`

### Tests

- **C++ sqllogictest**: `test/sql/decide_explain.test` — 30+ regex-based checks covering all EXPLAIN formats on inline data
- **Python pytest (TPC-H)**: `test/decide/tests/test_explain.py` — 21 tests covering EXPLAIN on real TPC-H tables with WHEN/PER edge cases
