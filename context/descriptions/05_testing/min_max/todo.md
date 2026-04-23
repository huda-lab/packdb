# MIN/MAX Aggregate Test Coverage — Todo

## Missing coverage

### Composed MIN/MAX follow-ups (v2)

v1 ships composed MIN/MAX in additive LHS/objective with the *easy* direction
only (see `done.md` for v1 scope). v2 follow-up shapes — all currently
rejected at bind time, all pinned by negative tests in
`test/decide/tests/test_min_max.py` so the pins trip when v2 lands:

- **Hard-direction composed MIN/MAX** — `MAXIMIZE MAX(a) + MAX(b)` or
  `MAXIMIZE SUM + MAX(a) >= K`. Requires per-row binary indicators and
  `SUM(y) >= 1` pinning per term. Pinned by `test_composed_minmax_hard_rejected`
  and `test_composed_minmax_objective_hard_rejected`.
- **Composed MIN/MAX with subtraction** (`MAX - MIN <= K`). Requires
  direction-flipping per term. Pinned by `test_composed_minmax_subtraction_rejected`.
- **Scalar-multiplied composed terms** (`2 * MIN(...) + SUM(...)`). Pinned
  by `test_composed_minmax_scalar_mult_rejected` (current rejection comes
  from the upstream Big-M MIN limitation after the K*WHEN fold normalises
  the scalar multiplier).
- **Composed MIN/MAX with outer `PER`** (`MIN(...) + MIN(...) <= K PER grp`).
  Pinned by `test_composed_minmax_per_wrapper_rejected`.
- **Composed MIN/MAX with non-constant RHS**. Pinned by two tests —
  `test_composed_minmax_nonconst_rhs_subquery_rejected` (clean DECIDE-specific
  error: `Composed MIN/MAX in DECIDE v1 requires a constant RHS`) and
  `test_composed_minmax_nonconst_rhs_column_rejected` (generic SUM
  comparison error). Both pinned because they trip different binder paths.
- **Composed MIN/MAX with outer `WHEN`**. Pinned by
  `test_composed_minmax_outer_when_rejected` — the rejection path is the
  expression-vs-aggregate-local WHEN guard (distinct from the PER-pin's
  outer-WHEN/PER guard).

## Cross-references

- `PER + hard MIN/MAX` — see also `per/done.md`
- `entity_scope + hard MIN/MAX` — see also `entity_scope/done.md`
- Composed MIN/MAX expressivity — see `03_expressivity/such_that/done.md`
  and `03_expressivity/maximize_minimize/done.md`
