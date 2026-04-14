---
name: packdb-test-review
description: PackDB test coverage audit workflow. Use when a user invokes `$packdb-test-review` to find DECIDE coverage gaps, missing feature interactions, and untested edge cases.
---

# PackDB Test Review Workflow

Use this workflow when the latest user request invokes `$packdb-test-review`. Parse the text after `$packdb-test-review` as arguments.

This workflow is designed for independent audit passes. If subagents are available and allowed by the active Codex delegation rules, use them for the three audit roles. If not, perform the same three passes locally and state that no subagents were used.

## Arguments

- Empty: full audit.
- Feature name such as `per`, `when`, `min_max`, `entity_scope`, or `quadratic`: audit only that area.
- `interactions`: focus on cross-feature interactions.
- `errors`: focus on error handling coverage.

## Procedure

1. Gather the documented expressivity surface:
   - `context/descriptions/03_expressivity/README.md`
   - `context/descriptions/03_expressivity/decide/done.md`
   - `context/descriptions/03_expressivity/such_that/done.md`
   - `context/descriptions/03_expressivity/when/done.md`
   - `context/descriptions/03_expressivity/per/done.md`
   - `context/descriptions/03_expressivity/maximize_minimize/done.md`
   - `context/descriptions/03_expressivity/sql_functions/done.md`
   - `context/descriptions/03_expressivity/problem_types/done.md`
   - `context/descriptions/04_optimizer/rewrite_passes/done.md`
   - `AGENTS.md`

2. Gather current test coverage:
   - `test/decide/pytest.ini`
   - `test/decide/conftest.py`
   - All relevant files in `test/decide/tests/`

3. Build a mental coverage matrix:
   - Variable types and table-scoped variables.
   - Constraint operators.
   - Aggregates.
   - `WHEN`.
   - `PER`.
   - Quadratic and bilinear objectives.
   - Error handling.
   - Solver-specific restrictions.

4. Run three independent audit passes:
   - Single-Feature Gap Finder: check each documented feature on its own for missing basic, edge, objective, constraint, and error coverage.
   - Interaction Gap Finder: check pairs and triples such as `WHEN + PER`, `PER + MIN/MAX`, `entity_scope + PER`, `QP + WHEN`, and mixed variable types.
   - Edge Case and Data Shape Auditor: check zero rows, single rows, infeasible/unbounded problems, all-zero and negative coefficients, NULLs, asymmetric groups, fan-out joins, and multiple mixed constraints.

5. Synthesize gaps:
   - Deduplicate overlapping gaps.
   - Prioritize HIGH for silent math errors, optimizer rewrites, linearizations, and crash risks.
   - Prioritize MEDIUM for documented features or error cases without coverage.
   - Prioritize LOW for unlikely edge cases.

6. Output:
   - Scope and date.
   - Tests audited count and file count.
   - High-risk gaps with example queries.
   - Medium-risk and low-risk gaps.
   - Interaction matrix summary.
   - Prioritized recommended tests and target test files.

Do not write tests unless the user asks for implementation after the audit.

