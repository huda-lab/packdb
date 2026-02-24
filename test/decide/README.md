# DECIDE Test Framework

Pytest-based testing for PackDB's DECIDE clause. Each correctness test has a
hand-written Python oracle that builds an ILP model using gurobipy or highspy
directly — no SQL parsing in the oracle.

## Quick Start

```bash
# From the repo root — creates .venv, installs deps, runs all tests:
./test/decide/run_tests.sh

# Or from the test/decide directory:
./run_tests.sh
```

The `run_tests.sh` script automatically creates a virtualenv at
`test/decide/.venv/` on first run, installs all dependencies (including
packdb in editable mode), and then invokes pytest.

## Setup (Manual)

```bash
# Create virtualenv
python3 -m venv test/decide/.venv
source test/decide/.venv/bin/activate

# Install dependencies
pip install -r test/decide/requirements.txt

# Install packdb (editable, for development)
cd tools/pythonpkg && pip install -e . && cd ../..

# Ensure packdb.db exists (TPC-H data)
# Either build PackDB or set PACKDB_DB_PATH
```

## Running Tests

```bash
# Run all tests (via runner script)
./test/decide/run_tests.sh

# Run by category marker
./test/decide/run_tests.sh -m var_boolean
./test/decide/run_tests.sh -m cons_aggregate
./test/decide/run_tests.sh -m "error"
./test/decide/run_tests.sh -m "correctness"

# Run a specific test file
./test/decide/run_tests.sh -k test_var_boolean

# Run with performance output (-s ensures it's not captured)
./test/decide/run_tests.sh -s

# Skip large-scale tests
./test/decide/run_tests.sh -m "not large_scale"

# Just set up the virtualenv without running tests
./test/decide/run_tests.sh --setup-only
```

## Test Categories

| Marker | What it tests |
|--------|--------------|
| `var_boolean` | IS BOOLEAN decision variables |
| `var_integer` | IS INTEGER / default type |
| `cons_aggregate` | SUM-based constraints |
| `cons_perrow` | Per-row variable bounds |
| `cons_mixed` | Aggregate + per-row combined |
| `cons_between` | BETWEEN constraints |
| `cons_subquery` | Subquery on constraint RHS |
| `cons_multi` | Multiple constraints |
| `obj_maximize` | MAXIMIZE objective |
| `obj_minimize` | MINIMIZE objective |
| `obj_complex` | Complex coefficient arithmetic |
| `sql_joins` | JOINs with DECIDE |
| `error_parser` | Parser syntax errors |
| `error_binder` | Binder semantic errors |
| `error_infeasible` | Infeasible models |
| `per_clause` | PER keyword (xfail) |
| `large_scale` | Performance / scaling |
| `correctness` | All oracle comparison tests |
| `error` | All error tests |
| `performance` | All performance tests |

## Adding a New Test

1. Choose the appropriate test file based on the primary feature being tested
2. Follow the existing pattern:
   - Run the DECIDE query via `packdb_conn`
   - Fetch the same data via `duckdb_conn` (plain SQL, no DECIDE)
   - Build an oracle model using `oracle_solver`
   - Compare with `assert_optimal_match`
   - Record performance with `perf_tracker`
3. Add appropriate markers

## Performance Results

After each run, a summary table is printed and a JSON file is saved to
`results/perf_YYYYMMDD_HHMMSS.json`. Historical results can be compared
manually or with future tooling.

## Architecture

```
test/decide/
├── conftest.py          # Fixtures (connections, solver, perf tracker)
├── solver/              # Solver abstraction (Gurobi / HiGHS)
├── comparison/          # Objective comparison logic
├── performance/         # Perf tracking and reporting
├── results/             # JSON output (gitignored)
└── tests/               # All test files by category
```
