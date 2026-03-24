# Performance Benchmarking

This document describes the benchmarking infrastructure for measuring PackDB DECIDE query performance.

## Overview

The benchmark suite measures **wall-clock time**, **peak memory (RSS)**, and **per-stage breakdowns** across a set of DECIDE queries at multiple data scales. It is designed to:

- Establish baselines before optimization work
- Validate that optimizations produce measurable improvements
- Identify bottlenecks (solver vs model construction vs optimizer)
- Track regressions across commits

## Running Benchmarks

```bash
make decide-bench                              # Full run (all queries, 5 iterations, stage timers)
python3 benchmark/decide/run_benchmarks.py     # Same, without stage timers

# Subset of queries or custom scales
python3 benchmark/decide/run_benchmarks.py --queries Q1,Q5 --scales 50,500

# More iterations for statistical confidence
python3 benchmark/decide/run_benchmarks.py --iterations 10

# Compare against a previous run
python3 benchmark/decide/run_benchmarks.py --compare benchmark/decide/results/prev.json
```

## Stage Timers (C++ Instrumentation)

When `PACKDB_BENCH=1` is set (automatically by `make decide-bench`), the DECIDE pipeline emits per-stage timing to stderr:

```
PACKDB_BENCH: optimizer_ms=0.01        # DecideOptimizer rewrite passes
PACKDB_BENCH: model_construction_ms=32  # ILP model building (constraint generation, WHEN/PER eval)
PACKDB_BENCH: solver_ms=1448            # SolveILP() call (Gurobi or HiGHS)
PACKDB_BENCH: total_variables=9965      # num_rows * num_decide_vars
PACKDB_BENCH: total_constraints=5       # per-row + global constraints
PACKDB_BENCH: num_rows=9965
```

The Python script automatically parses these and includes them in the output.

**Source locations:**
- `src/execution/operator/decide/physical_decide.cpp` — model_construction_ms, solver_ms, total_variables, total_constraints, num_rows
- `src/optimizer/decide/decide_optimizer.cpp` — optimizer_ms

Timers use DuckDB's `Profiler` class (`src/include/duckdb/common/profiler.hpp`). Gated behind `std::getenv("PACKDB_BENCH")` — zero overhead when not set.

## Benchmark Query Set

Five queries, each run at 3 data scales (15 total runs). All use TPC-H SF=0.01 lineitem table with `WHERE l_orderkey < {SCALE}` to control row count.

| Query | Name | Features Exercised | Optimizer Passes | Scales |
|-------|------|--------------------|------------------|--------|
| Q1 | Knapsack baseline | IS BOOLEAN, SUM constraint + objective | None | 10, 100, 500 |
| Q2 | ABS + hard MIN/MAX | IS REAL, ABS linearization, hard MAX>=K | RewriteAbs, RewriteMinMax | 10, 50, 200 |
| Q3 | COUNT + AVG + WHEN | IS INTEGER, COUNT indicator, AVG scaling, WHEN filter | RewriteCountToSum, RewriteAvgToSum | 10, 100, 500 |
| Q4 | Nested PER | IS BOOLEAN, PER grouping, nested MINIMIZE MAX(SUM(...)) PER | RewriteMinMax (easy) | 10, 50, 200 |
| Q5 | Stress test | IS BOOLEAN, 3 SUM constraints, large variable count | None | 500, 2000, 10000 |

### Coverage Matrix

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 |
|---------|:--:|:--:|:--:|:--:|:--:|
| IS BOOLEAN | x | | | x | x |
| IS INTEGER | | | x | | |
| IS REAL | | x | | | |
| ABS linearization | | x | | | |
| MIN/MAX (hard Big-M) | | x | | | |
| MIN/MAX (easy) | | | | x | |
| COUNT->SUM rewrite | | | x | | |
| AVG->SUM rewrite | | | x | | |
| WHEN filtering | | | x | | |
| PER grouping | | | | x | |
| Nested aggregates | | | | x | |
| Multiple constraints | | x | x | x | x |

### Query SQL

Templates are in `benchmark/decide/queries/*.sql.template` with `{SCALE}` placeholder.

**Q1 — Knapsack baseline:** Pure binary knapsack. No optimizer rewrites. Baseline for pipeline + solver overhead.
```sql
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, x
FROM lineitem WHERE l_orderkey < {SCALE}
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_quantity) <= 500
MAXIMIZE SUM(x * l_extendedprice);
```

**Q2 — ABS + hard MIN/MAX:** Exercises heaviest optimizer rewrites. ABS creates auxiliary REAL vars + linearization constraints. Hard MAX creates Big-M indicator variables.
```sql
SELECT l_orderkey, l_linenumber, l_quantity, new_qty
FROM lineitem WHERE l_orderkey < {SCALE}
DECIDE new_qty IS REAL
SUCH THAT SUM(ABS(new_qty - l_quantity)) <= 200
    AND MAX(new_qty) >= 40
    AND new_qty <= 50
MINIMIZE SUM(ABS(new_qty - l_quantity));
```

**Q3 — COUNT(integer) + AVG + WHEN:** COUNT on INTEGER creates indicator vars + linking constraints. AVG scales RHS by row count. WHEN filters rows in execution.
```sql
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
       l_discount, l_returnflag, x
FROM lineitem WHERE l_orderkey < {SCALE}
DECIDE x
SUCH THAT x <= 5 AND COUNT(x) >= 3
    AND AVG(x * l_discount) <= 0.25
    AND SUM(x * l_quantity) <= 200 WHEN l_returnflag = 'R'
MAXIMIZE SUM(x * l_extendedprice);
```

**Q4 — Nested PER:** PER grouping on constraints and nested aggregate objective. MINIMIZE MAX is a load-balancing objective (global auxiliary z with per-group linking).
```sql
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
       l_returnflag, x
FROM lineitem WHERE l_orderkey < {SCALE}
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 2 PER l_returnflag
    AND SUM(x * l_quantity) <= 100 PER l_returnflag
    AND SUM(x) >= 5
MINIMIZE MAX(SUM(x * l_extendedprice)) PER l_returnflag;
```

**Q5 — Stress test:** Multi-dimensional knapsack (3 resource constraints). Simple model structure but large variable count to stress the solver.
```sql
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
       l_discount, x
FROM lineitem WHERE l_orderkey < {SCALE}
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_quantity) <= 5000
    AND SUM(x) <= 500
    AND SUM(x * l_extendedprice * l_discount) <= 50000
MAXIMIZE SUM(x * l_extendedprice * (1 - l_discount));
```

## Output

### Terminal Table
```
PackDB Benchmark Results (commit: 6b56b35)
==============================================================================
Query                      Scale  Median(s)   StdDev  PeakRSS(MB)
------------------------------------------------------------------------------
Q1_knapsack_baseline          10     0.0800   0.0100         31.3
                                   stages: model_construction_ms=0.2, solver_ms=49.6, ...
Q5_stress                  10000     1.5300   0.0460        103.4
                                   stages: model_construction_ms=32.5, solver_ms=1447.7, ...
==============================================================================
```

### JSON Output
Saved to `benchmark/decide/results/<commit>_<timestamp>.json` with full per-run data, system info, and stage breakdowns. Gitignored.

## Baseline Observations

Initial baseline (commit 6b56b35, HiGHS solver):

- **Solver dominates**: At Q5/10K rows, solver takes ~1448ms out of ~1530ms total (95%). Model construction is ~33ms (2%). Optimizer is negligible (<0.01ms).
- **Memory scales linearly**: ~30MB base + ~7KB per row at Q5/10K scale.
- **Q1-Q4 are fast**: All under 110ms even at largest scale — the solver handles these problem sizes easily.
- **Optimization focus**: Reducing solver input size (fewer variables/constraints) or improving formulations (tighter bounds) will have the most impact. Code-level optimizations in model construction only matter for very large problems.

## File Layout

```
benchmark/decide/
├── run_benchmarks.py           # Orchestration script
├── queries/
│   ├── q1_knapsack_baseline.sql.template
│   ├── q2_abs_minmax.sql.template
│   ├── q3_count_avg_when.sql.template
│   ├── q4_nested_per.sql.template
│   └── q5_stress.sql.template
├── results/                    # JSON outputs (gitignored)
└── .gitignore
```
