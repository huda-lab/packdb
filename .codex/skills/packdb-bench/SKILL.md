---
name: packdb-bench
description: PackDB benchmark workflow. Use when a user invokes `$packdb-bench` to build if needed, run DECIDE benchmarks, view results, analyze stage timings, and suggest the next optimization.
---

# PackDB Benchmark Workflow

Use this workflow when the latest user request invokes `$packdb-bench`. Parse the text after `$packdb-bench` as arguments.

## Arguments

- Empty: build if needed, benchmark `medium`, compare with previous results.
- `full`: benchmark `small,medium,large`.
- `small`, `medium`, `large`, or comma-separated sizes: benchmark those sizes.
- `manual`: run `benchmark/decide/queries/manual.sql`.
- `compare`: force comparison.
- `skip-build`: skip `make release`.

## Procedure

1. Parse arguments:
   - `sizes = medium` by default.
   - `do_build = false` only when `skip-build` is present.
   - `do_compare = true` by default and always true when `compare` is present.
   - `is_manual = true` when `manual` is present.

2. Build unless skipped:
   - If `build/release/packdb` is missing or older than files under `src/`, run `make release`.
   - Stop on build failure.

3. Ensure benchmark databases exist:
   - Check `benchmark/decide/databases/` for required size databases.
   - If any required database is missing, run `make decide-bench-setup`.

4. Run benchmarks:
   - Standard: `PACKDB_BENCH=1 python3 benchmark/decide/run_benchmarks.py --sizes {sizes} --compare`
   - Manual: `PACKDB_BENCH=1 python3 benchmark/decide/run_benchmarks.py --manual --compare`
   - Omit `--compare` only if the workflow explicitly decides comparison is not requested.

5. View results:
   - Run `python3 benchmark/decide/view_results.py`.

6. Analyze the latest result JSON in `benchmark/decide/results/`:
   - Compute `solver_ms / (wall_time_s * 1000)`.
   - Compute `model_construction_ms / (wall_time_s * 1000)`.
   - Compute `optimizer_ms / (wall_time_s * 1000)`.
   - Present a compact table with query, size, rows, vars, constraints, wall time, Solver%, Model%, and Opt%.

7. Suggest one next optimization:
   - Tie the suggestion to observed time, variable count, constraint count, or scaling.
   - Explain what to change, why it matters, the ILP/formulation mechanism, and where in the code to look.
   - Prefer concrete PackDB paths such as `src/optimizer/decide/decide_optimizer.cpp`, `src/packdb/utility/ilp_model_builder.cpp`, and `src/execution/operator/decide/physical_decide.cpp`.

