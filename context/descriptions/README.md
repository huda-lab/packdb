# PackDB Documentation Index

This folder contains all internal documentation for the PackDB project. It is
structured for quick navigation by both AI agents and human developers.

---

## Folder Map

| Folder                 | Contains                                                                                                           | Start here if...                                                              |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------- |
| `00_project_overview/` | What PackDB is, the theory behind it, and the DECIDE syntax reference                                              | You are new to the project or need to understand what queries are valid       |
| `01_pipeline/`         | The query processing pipeline: architecture, each stage in order, source-code map, and a concrete end-to-end trace | You are working on or debugging any part of the DECIDE query path             |
| `02_operations/`       | Testing methodology, release workflow, and known limitations                                                       | You need to run tests, cut a release, or check whether a feature is supported |

---

## Recommended Reading Order (Starting From Scratch)

1. `00_project_overview/project_description.md` — What PackDB is and why it exists.
2. `00_project_overview/syntax_reference.md` — What you can write in a DECIDE clause.
3. `01_pipeline/architecture.md` — How the system is structured and what the stages are.
4. `01_pipeline/trace_life_of_a_query.md` — A concrete example walking through every stage with real data.
5. The individual stage docs (`01_parser.md`, `02_binder.md`, `03_execution.md`) as needed for the specific stage you are working on.

You do not need to read `00_project_overview/background_theory.md` unless you want the
academic motivation (ILP, Package Queries). You do not need to read
`01_pipeline/code_structure.md` unless you are about to modify source code and need to
know where things live on disk.

---

## Quick Lookup: Pipeline Stage → Doc → Source

The DECIDE clause passes through three custom stages after standard DuckDB parsing:

| Stage             | What it does                                                              | Doc                           | Key source file(s)                                                                 |
| ----------------- | ------------------------------------------------------------------------- | ----------------------------- | ---------------------------------------------------------------------------------- |
| Parser / Symbolic | Normalizes algebraic expressions into canonical linear form               | `01_pipeline/01_parser.md`    | `src/packdb/symbolic/decide_symbolic.cpp`                                          |
| Binder            | Validates linearity, binds decision variables, resolves types             | `01_pipeline/02_binder.md`    | `src/planner/expression_binder/decide_binder.cpp`, `decide_constraints_binder.cpp` |
| Execution         | Materializes data, builds the solver matrix, runs HiGHS, projects results | `01_pipeline/03_execution.md` | `src/execution/operator/decide/physical_decide.cpp`                                |

For the full source-tree map (all headers, all classes, all key methods), see
`01_pipeline/code_structure.md`.

---

## Note on Redundancy

A few topics appear in more than one file. This is intentional — each mention is
scoped to that file's audience. Authoritative sources:

- **DECIDE syntax** — `00_project_overview/syntax_reference.md` is the full spec.
  `project_description.md` has a brief teaser; `trace_life_of_a_query.md` shows it in
  the context of a real query.
- **IS BOOLEAN / IS INTEGER** — `syntax_reference.md` defines what the user writes.
  `02_binder.md` explains what the binder does with that declaration internally.
- **Linearity constraint** — appears in theory, parser, binder, and syntax docs, each
  scoped to that stage. The user-facing spec is in `syntax_reference.md`. The
  implementation check is in `02_binder.md`.
