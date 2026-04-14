---
name: packdb-learn
description: PackDB learning workflow. Use when a user invokes `$packdb-learn` to understand a DECIDE feature, pipeline stage, concept, or source file.
---

# PackDB Learn Workflow

Use this workflow when the latest user request invokes `$packdb-learn`. Parse the text after `$packdb-learn` as the required topic.

If the topic is missing, ask what they want to learn about and suggest examples like `$packdb-learn PER`, `$packdb-learn Big-M`, or `$packdb-learn pipeline`.

## Source Mapping

- Feature topics such as `PER`, `WHEN`, `COUNT`, `AVG`, `MIN/MAX`, `QP`, `ABS`, and `indicators`: read the matching area under `context/descriptions/03_expressivity/`.
- Parser: read `context/descriptions/01_pipeline/01_parser.md` and `src/packdb/symbolic/decide_symbolic.cpp`.
- Binder: read `context/descriptions/01_pipeline/02_binder.md` and `src/planner/expression_binder/decide_binder.cpp`.
- Optimizer, Big-M, linearization, easy vs hard, or auxiliary variables: read `context/descriptions/04_optimizer/` and `src/optimizer/decide/decide_optimizer.cpp`.
- Execution or solver: read `context/descriptions/01_pipeline/03_execution.md`, the execution sub-docs, `src/execution/operator/decide/physical_decide.cpp`, and solver backends under `src/packdb/solver/`.
- File path topics: read the file directly.

Always read `context/descriptions/README.md` for navigation and `AGENTS.md` for the quick syntax reference.

## Output

Use four sections:

1. `The Idea`: 2-3 sentences explaining what the topic is and why it exists.
2. `How It Works`: trace a simple example through the relevant pipeline stages. Use short real C++ snippets with file paths and line numbers when helpful.
3. `Example`: show one complete minimal DECIDE query and explain what it asks for.
4. `Where in the Code`: list the key files and docs to read next.

## Tone

Build a mental model, not just a source map. Define jargon inline. Use real code snippets only; SQL examples may be illustrative.

