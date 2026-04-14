---
name: packdb-docs-review
description: PackDB documentation accuracy audit workflow. Use when a user invokes `$packdb-docs-review` to audit context/descriptions and agent docs against the actual codebase.
---

# PackDB Docs Review Workflow

Use this workflow when the latest user request invokes `$packdb-docs-review`. Parse the text after `$packdb-docs-review` as arguments.

This workflow is designed for independent audit passes. If subagents are available and allowed by the active Codex delegation rules, use them for the three audit roles. If not, perform the same three passes locally and state that no subagents were used.

## Arguments

- Empty: audit all `context/descriptions/`, `AGENTS.md`, and `.codex/lessons.md`.
- Expressivity area such as `decide`, `such_that`, `when`, `per`, `maximize_minimize`, `problem_types`, or `sql_functions`: audit the matching `context/descriptions/03_expressivity/` subdirectory.
- Optimizer area such as `optimizer`, `rewrite_passes`, `partition_solve`, or `matrix_efficiency`: audit `context/descriptions/04_optimizer/`.
- Pipeline area such as `pipeline`, `parser`, `binder`, or `execution`: audit `context/descriptions/01_pipeline/`.
- `codex`: audit only `AGENTS.md` and `.codex/`.

## Procedure

1. Determine scope from arguments.

2. Run three independent audit passes:
   - Code Pointer Verifier: verify file paths, function names, line references, constants, tags, fields, enum values, and grammar rule references.
   - Content Accuracy Verifier: verify behavioral claims, syntax claims, restrictions, rewrite descriptions, and quoted error messages against the actual code.
   - Completeness and Consistency Auditor: check implemented-but-undocumented behavior, stale `todo.md` items, contradictions across docs, navigation gaps, stale lessons, and invalid examples.

3. Use targeted searches:
   - Prefer `rg` and `rg --files`.
   - Read code around claimed functions or line references before deciding a pointer is stale.
   - For syntax claims, cross-check grammar, parser, binder, and optimizer paths.

4. Synthesize findings:
   - CRITICAL: docs actively mislead or claim unsupported syntax/behavior.
   - STALE: old but not dangerous pointers or descriptions.
   - GAP: implemented behavior or navigation that is missing from docs.
   - COSMETIC: wording and formatting issues.

5. Output:
   - Scope, date, and docs checked count.
   - Critical issues first with evidence and fix.
   - Stale content, gaps, and cosmetic issues.
   - Cross-cutting observations.
   - Recommended fix order.

Do not edit docs unless the user asks for implementation after the audit.

