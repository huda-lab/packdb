# Performance Benchmarking

This document describes the benchmarking infrastructure for measuring PackDB DECIDE query performance.

## Overview

The benchmark suite measures **wall-clock time**, **peak memory (RSS)**, and **per-stage breakdowns** across a set of DECIDE queries at multiple database sizes. It is designed to:

- Establish baselines before optimization work
- Validate that optimizations produce measurable improvements
- Identify bottlenecks (solver vs model construction vs optimizer)
- Track regressions across commits

## Running Benchmarks

```bash
make decide-bench-setup                           # Generate databases (one-time)
make decide-bench                                  # Full run (all queries, all sizes, stage timers)
make decide-bench-manual                           # Run manual query
make decide-view                                   # View latest results

# Subset of queries or sizes
python3 benchmark/decide/run_benchmarks.py --queries Q1,Q5 --sizes small,medium

# More iterations for statistical confidence
python3 benchmark/decide/run_benchmarks.py --iterations 10

# Compare against previous commit (auto-detect from git log)
python3 benchmark/decide/run_benchmarks.py --compare

# Compare against specific commit
python3 benchmark/decide/run_benchmarks.py --compare abc1234

# View specific results
python3 benchmark/decide/view_results.py {hash}
python3 benchmark/decide/view_results.py dirty
python3 benchmark/decide/view_results.py manual
```

## Database Sizes

Three pre-generated TPC-H databases at different scale factors:

| Size | TPC-H SF | ~lineitem rows | Purpose |
|------|----------|---------------|---------|
| small | 0.01 | ~60K | Fast iteration, CI |
| medium | 0.1 | ~600K | Moderate stress testing |
| large | 1.0 | ~6M | Full-scale benchmarking |

Generate with `make decide-bench-setup` (runs `generate_databases.py`). Databases are stored in `benchmark/decide/databases/` (gitignored). Existing databases are skipped — delete to regenerate.

## Stage Timers (C++ Instrumentation)

When `PACKDB_BENCH=1` is set (automatically by `make decide-bench`), the DECIDE pipeline emits per-stage timing to stderr:

```
PACKDB_BENCH: optimizer_ms=0.01        # DecideOptimizer rewrite passes
PACKDB_BENCH: model_construction_ms=32  # ILP model building (constraint generation, WHEN/PER eval)
PACKDB_BENCH: solver_ms=1448            # SolveModel() call (Gurobi or HiGHS)
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

Five queries, each run at all database sizes. Queries use the full lineitem table (no row filtering).

| Query | Name | Features Exercised | Optimizer Passes |
|-------|------|--------------------|------------------|
| Q1 | Knapsack baseline | IS BOOLEAN, SUM constraint + objective | None |
| Q2 | ABS + hard MIN/MAX | IS REAL, ABS linearization, hard MAX>=K | RewriteAbs, RewriteMinMax |
| Q3 | AVG + WHEN | IS INTEGER, AVG scaling, WHEN filter | RewriteAvgToSum |
| Q4 | Nested PER | IS BOOLEAN, PER grouping, nested MINIMIZE MAX(SUM(...)) PER | RewriteMinMax (easy) |
| Q5 | Stress test | IS BOOLEAN, 3 SUM constraints, large variable count | None |

### Coverage Matrix

| Feature | Q1 | Q2 | Q3 | Q4 | Q5 |
|---------|:--:|:--:|:--:|:--:|:--:|
| IS BOOLEAN | x | | | x | x |
| IS INTEGER | | | x | | |
| IS REAL | | x | | | |
| ABS linearization | | x | | | |
| MIN/MAX (hard Big-M) | | x | | | |
| MIN/MAX (easy) | | | | x | |
| AVG->SUM rewrite | | | x | | |
| WHEN filtering | | | x | | |
| PER grouping | | | | x | |
| Nested aggregates | | | | x | |
| Multiple constraints | | x | x | x | x |

### Query SQL

Queries are in `benchmark/decide/queries/*.sql`.

**Q1 — Knapsack baseline:** Pure binary knapsack. No optimizer rewrites. Baseline for pipeline + solver overhead.
```sql
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, x
FROM lineitem
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_quantity) <= 500
MAXIMIZE SUM(x * l_extendedprice);
```

**Q2 — ABS + hard MIN/MAX:** Exercises heaviest optimizer rewrites. ABS creates auxiliary REAL vars + linearization constraints. Hard MAX creates Big-M indicator variables.
```sql
SELECT l_orderkey, l_linenumber, l_quantity, new_qty
FROM lineitem
DECIDE new_qty IS REAL
SUCH THAT SUM(ABS(new_qty - l_quantity)) <= 200
    AND MAX(new_qty) >= 40
    AND new_qty <= 50
MINIMIZE SUM(ABS(new_qty - l_quantity));
```

**Q3 — AVG + WHEN:** AVG scales RHS by row count. WHEN filters rows in execution.
```sql
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
       l_discount, l_returnflag, x
FROM lineitem
DECIDE x
SUCH THAT x <= 5
    AND AVG(x * l_discount) <= 0.25
    AND SUM(x * l_quantity) <= 200 WHEN l_returnflag = 'R'
MAXIMIZE SUM(x * l_extendedprice);
```

**Q4 — Nested PER:** PER grouping on constraints and nested aggregate objective. MINIMIZE MAX is a load-balancing objective (global auxiliary z with per-group linking).
```sql
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice,
       l_returnflag, x
FROM lineitem
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
FROM lineitem
DECIDE x IS BOOLEAN
SUCH THAT SUM(x * l_quantity) <= 5000
    AND SUM(x) <= 500
    AND SUM(x * l_extendedprice * l_discount) <= 50000
MAXIMIZE SUM(x * l_extendedprice * (1 - l_discount));
```

## Output

### Visual Output

After each run, `view_results.py` displays colored stage-proportion bars:

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Q1: knapsack_baseline
  SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, x
  FROM lineitem
  DECIDE x IS BOOLEAN
  SUCH THAT SUM(x * l_quantity) <= 500
  MAXIMIZE SUM(x * l_extendedprice);

  small  │ 60K rows │ 60K vars │ 3 constraints │ 0.45s
  ██░░░░░░░░░░░░████████████████████████████████████░░░░░░
   opt       model              solver              other
```

Stage colors: optimizer (blue), model construction (yellow), solver (red), overhead (grey). Bar width: 60 characters, proportional to time share.

The viewer can be run standalone:
```bash
python3 benchmark/decide/view_results.py           # latest result
python3 benchmark/decide/view_results.py {hash}    # specific commit
python3 benchmark/decide/view_results.py dirty     # dirty result
python3 benchmark/decide/view_results.py manual    # manual result
```

### JSON Output

Saved to `benchmark/decide/results/{commit}.json` (or `dirty.json` for uncommitted changes, `manual.json` for manual queries).

```json
{
  "commit": "6b56b35",
  "timestamp": "...",
  "system": {...},
  "iterations": 5,
  "sizes": ["small", "medium", "large"],
  "queries": [
    {
      "query": "Q1",
      "description": "knapsack_baseline",
      "size": "small",
      "sql": "SELECT l_orderkey ...",
      "runs": [...],
      "stats": {
        "median_wall_time_s": 0.45,
        "stddev_wall_time_s": 0.02,
        "median_peak_rss_kb": 32000,
        "stages": {
          "optimizer_ms": 0.01,
          "model_construction_ms": 5.2,
          "solver_ms": 420.0,
          "total_variables": 60173,
          "total_constraints": 3,
          "num_rows": 60173
        }
      }
    }
  ]
}
```

Running on the same commit overwrites the previous result file (deterministic naming eliminates deduplication).

### Manual Queries

For ad-hoc benchmarking:
1. Copy `queries/manual.sql.example` to `queries/manual.sql`
2. Edit the query
3. Run `make decide-bench-manual`

Results saved to `results/manual.json` (always overwritten). No commit tracking or comparison.

### Comparison

Use `--compare` to see deltas between the current run and a previous one:

```bash
# Auto-detect: walks git log to find the most recent commit with results
python3 benchmark/decide/run_benchmarks.py --compare

# Explicit: compare against a specific commit hash
python3 benchmark/decide/run_benchmarks.py --compare abc1234
```

## Baseline Observations

Initial baseline (commit 6b56b35, HiGHS solver, small database):

- **Solver dominates**: At Q5, solver takes ~95% of wall time. Model construction is ~2%. Optimizer is negligible (<0.01ms).
- **Memory scales linearly**: ~30MB base + ~7KB per row at large scale.
- **Q1-Q4 are fast**: All under 110ms on small database — the solver handles these problem sizes easily.
- **Optimization focus**: Reducing solver input size (fewer variables/constraints) or improving formulations (tighter bounds) will have the most impact. Code-level optimizations in model construction only matter for very large problems.

## Makefile Targets

| Target | Description |
|--------|-------------|
| `make decide-bench-setup` | Generate TPC-H databases (small/medium/large) |
| `make decide-bench` | Run full benchmark suite with stage timers |
| `make decide-bench-manual` | Run manual query benchmark |
| `make decide-view` | View latest benchmark results |

## File Layout

```
benchmark/decide/
├── generate_databases.py      # Database generation script
├── run_benchmarks.py          # Orchestration script
├── view_results.py            # Visual results viewer
├── queries/
│   ├── q1_knapsack_baseline.sql
│   ├── q2_abs_minmax.sql
│   ├── q3_count_avg_when.sql
│   ├── q4_nested_per.sql
│   ├── q5_stress.sql
│   └── manual.sql.example
├── databases/                 # Generated TPC-H DBs (gitignored)
│   ├── small.db               # SF=0.01, ~60K rows
│   ├── medium.db              # SF=0.1,  ~600K rows
│   └── large.db               # SF=1.0,  ~6M rows
├── results/                   # JSON outputs (gitignored)
└── .gitignore
```
