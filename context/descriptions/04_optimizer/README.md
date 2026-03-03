# PackDB COP Optimizer

This folder documents the optimization strategies for Constrained Optimization Problem (COP) queries — the DECIDE clause portion of PackDB. Each strategy area is a **subfolder** with:

- `done.md` — What is implemented today, with code pointers
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

---

## Development Priorities

1. **Query rewriting** (Big-M reformulation, constraint push-down/pull-out) — **primary near-term focus**
2. **Problem reduction** (skyband indexing, progressive shading) — for scaling to large datasets
3. **Physical planning** (cost-based solver selection) — for production deployment
4. **Adaptive processing** (softening, incremental) — for interactive use cases

---

## Folders

| Folder | done.md | todo.md |
|---|---|---|
| [query_rewriting/](query_rewriting/) | WHERE filtering, WHEN coefficient zeroing | Big-M, push-down, pull-out, bound conversion |
| [reformulation/](reformulation/) | *(nothing)* | LP relaxation, constraint tightening, symmetry breaking |
| [problem_reduction/](problem_reduction/) | *(nothing)* | **Partition-solve (PER decomposition)**, Skyband indexing, Progressive Shading, LGS |
| [adaptive_processing/](adaptive_processing/) | *(nothing)* | Solver timeout, constraint softening, incremental reasoning |
| [physical_planning/](physical_planning/) | Solver selection (Gurobi/HiGHS) | Cost-based strategy selection |

---

## Summary: What's Implemented vs. What's Planned

| Strategy | Status |
|---|---|
| WHERE-clause filtering | Implemented (inherited from DuckDB) |
| WHEN-condition coefficient zeroing | Implemented |
| Solver selection (Gurobi/HiGHS fallback) | Implemented |
| Big-M reformulation | **Not implemented** |
| Constraint push-down | **Not implemented** |
| Constraint pull-out | **Not implemented** |
| Constraint-to-bound conversion | **Not implemented** |
| LP relaxation + rounding | **Not implemented** |
| Partition-solve (PER decomposition) | **Not implemented** (enabled by PER `row_group_ids` design) |
| Skyband indexing | **Not implemented** |
| Progressive Shading | **Not implemented** |
| Solver time limit | **Not implemented** |
| Everything else | **Not implemented** |

The current system performs **no COP-specific optimization** beyond basic filtering and solver dispatch. The full optimizer vision involves query rewriting, reformulation, problem reduction, adaptive processing, and cost-based physical planning.
