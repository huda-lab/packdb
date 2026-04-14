---
name: packdb-review
description: PackDB devil's advocate review workflow. Use when a user invokes `$packdb-review` to review local changes or recent commits across formulation correctness, DuckDB architecture, and documentation/test completeness.
---

# PackDB Review Workflow

Use this workflow when the latest user request invokes `$packdb-review`. Parse the text after `$packdb-review` as arguments.

This workflow is designed for independent reviewer passes. If subagents are available and allowed by the active Codex delegation rules, use them for the three reviewer roles. If not, perform the same three passes locally and state that no subagents were used.

## Arguments

- Empty: review staged and unstaged changes with `git diff HEAD`.
- `last N`: review `git diff HEAD~N..HEAD`.
- Any other text: treat as a scope hint.

## Procedure

1. Gather context:
   - Diff command from the arguments.
   - Changed file list.
   - `git log --oneline -10`.
   - If the diff is empty, stop and say there is nothing to review.

2. Classify changed files:
   - Source: `src/`
   - Tests: `test/`
   - Docs: `context/descriptions/`
   - Config: `AGENTS.md`, `.codex/`, `.claude/`, `Makefile`
   - Grammar: `third_party/libpg_query/`

3. Read relevant docs:
   - Always read `context/descriptions/README.md` and `AGENTS.md`.
   - Optimizer changes: `context/descriptions/04_optimizer/rewrite_passes/done.md`.
   - Execution changes: `context/descriptions/01_pipeline/03_execution.md` and sub-docs.
   - Binder changes: `context/descriptions/01_pipeline/02_binder.md`.
   - Parser or grammar changes: `context/descriptions/01_pipeline/01_parser.md`.

4. Run three independent review passes:
   - Formulation Reviewer: mathematical correctness, Big-M tightness/signs, rewrite equivalence, MIN/MAX easy/hard classification, solver-agnostic behavior, and adversarial cases.
   - Architecture Reviewer: DuckDB conventions, performance, minimal core modifications, memory safety, lifecycle, and error handling.
   - Completeness Reviewer: documentation drift, `AGENTS.md` sync, `.codex/lessons.md`, tests, grammar regeneration, and code pointer accuracy.

5. Synthesize findings:
   - Deduplicate by issue and file location.
   - Sort by severity: CRITICAL, WARNING, INFO.
   - Put findings first, with file/line evidence.
   - Include open questions only when they affect correctness.

6. Output:
   - Scope and commit/range.
   - Findings grouped by severity.
   - Cross-cutting observations.
   - Unresolved items requiring human judgment.
   - Recommended next actions in priority order.

## Review Standard

Take a code-review stance. Prioritize bugs, regressions, correctness risks, and missing tests. Do not bury findings in a long summary.

