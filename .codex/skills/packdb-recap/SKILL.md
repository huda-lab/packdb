---
name: packdb-recap
description: PackDB change recap workflow. Use when a user invokes `$packdb-recap` to explain current or recent code changes in plain English with relevant PackDB context.
---

# PackDB Recap Workflow

Use this workflow when the latest user request invokes `$packdb-recap`. Parse the text after `$packdb-recap` as arguments.

## Arguments

- Empty: explain staged and unstaged changes from `git diff HEAD`.
- `last N`: explain the last N commits with `git diff HEAD~N..HEAD`.
- Any other text: treat as a focus hint.

## Procedure

1. Gather context:
   - Diff: `git diff HEAD` or `git diff HEAD~N..HEAD`.
   - Changed files: `git diff --name-only HEAD` or matching commit range.
   - Recent commits: `git log --oneline -10`.
   - If the diff is empty, say there is nothing to recap.

2. Read relevant docs based on changed files:
   - `src/optimizer/decide/`: `context/descriptions/04_optimizer/rewrite_passes/done.md`
   - `src/execution/operator/decide/`: `context/descriptions/01_pipeline/03_execution.md`
   - `src/planner/expression_binder/`: `context/descriptions/01_pipeline/02_binder.md`
   - `src/packdb/symbolic/` or `third_party/libpg_query/`: `context/descriptions/01_pipeline/01_parser.md`
   - `test/decide/`: read the changed tests.

3. Group changes:
   - Grammar/Parser
   - Binder
   - Optimizer
   - Execution
   - Headers/Data Structures
   - Tests
   - Docs
   - Config/Build

4. Explain each group in plain English:
   - What changed.
   - Why it changed.
   - A short real snippet from the diff when useful.
   - How it connects to other changed groups.

5. End with a short TL;DR:
   - State the goal, the end result, and anything the user should remember.

## Tone

Explain like the reader is smart but unfamiliar with PackDB internals. Define unavoidable jargon inline. Prefer why over what. Keep snippets short and real.

