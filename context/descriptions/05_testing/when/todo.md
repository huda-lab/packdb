# WHEN Clause Test Coverage — Todo

## Missing coverage

### Empty WHEN on MIN/MAX constraints — silent (verified)

Surfaced during a systematic LHS audit. The following shapes return
`OPTIMAL` with the constraint silently ignored, even though the
mathematical semantics make them infeasible:

- **Non-composed hard direction**:
  `(MAX(x * v) WHEN <never-true>) >= K`,
  `(MIN(x * v) WHEN <never-true>) <= K`
- **Composed (any direction)**:
  `SUM(x * v) + (MAX(x * v) WHEN FALSE) <= K` silently vacates the
  entire composed constraint (not just the MAX term). Verified: baseline
  `SUM(x * v) <= 8` selects one row, while the composed form with the
  dead MAX term selects all rows (SUM greatly exceeds the bound).

Non-composed easy direction (`MAX <= K`, `MIN >= K`) is correctly handled
by per-row emission — empty filter → zero constraints.

Coverage: add an empty-WHEN probe matrix for **constraints** mirroring the
existing `test_edge_cases.py::test_maximize_min_objective_when_empty`
objective-side matrix, including the composed-easy case. Tests currently
do not exercise any constraint-side empty-WHEN flavor. Shared root cause
tracked in `../min_max/todo.md`.

### `decide_when_condition` grammar restrictions — covered

Closed by `test/decide/tests/test_when_grammar.py`:

- **Positive parenthesized constraint tests** (3): `WHEN (NOT w)`, `WHEN (tier = 'high')`, `WHEN (a + b > 5)` — all oracle-verified.
- **Positive parenthesized objective tests** (3): same 3 shapes — all oracle-verified.
- **Negative unparenthesized constraint tests** (3): pin shape-specific parser errors per `../../03_expressivity/when/todo.md` table.
- **Negative unparenthesized objective tests** (2): `WHEN NOT w` (parser-level) and `WHEN a + b > 5` (binder-level). The third unparenthesized objective shape `WHEN tier = 'high'` is NOT a negative test because the reassociator handles it; that path is exercised in the parenthesized objective positive set.
- **Asymmetric-error sentinel** (1): `SUM(x*v) WHEN tier = 'high' <= 10` on a constraint pins the actual `syntax error at or near "<="` parser error. The earlier doc claim that this produces an `"LHS must be a DECIDE variable or SUM expression"` message was incorrect — that's a binder-level error path that doesn't fire here because the parser bails first.

If the grammar widens or a constraint-side reassociator is added, the sentinel test will fail; the appropriate response is to delete it and convert the case to a positive test, not to relax the regex. See `../../03_expressivity/when/todo.md` for the full asymmetry table.
