# Lessons Learned

Corrections and patterns discovered during development. Updated after mistakes to prevent recurrence.

## Rules

- **Grammar files are templates**: Never edit `src_backend_parser_gram.cpp` directly. Edit `.y`/`.yh` files in `third_party/libpg_query/grammar/`, then run `python3 scripts/generate_grammar.py`.
- **Build before testing**: Always `make` (or `make debug`) after grammar or source changes before running tests.

## Gotchas

<!-- Add entries here as they come up, format: short description + what to do instead -->
