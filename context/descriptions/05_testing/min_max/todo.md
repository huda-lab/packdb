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

### Empty WHEN on hard-direction MIN/MAX — silent infeasibility (objective AND constraint)

The hard-direction MIN/MAX reformulation uses a global continuous
auxiliary `z` plus per-row Big-M indicator variables pinning `z` to at
least one row's inner value. When a `WHEN` filter matches zero rows, the
auxiliary has no per-row linking constraints — it floats free inside its
bounds, so any constraint or objective over it is vacuously satisfied.
No error, no warning. Semantically `MIN(∅) = +∞`, `MAX(∅) = -∞`, so a
hard-direction bound should be **infeasible**, not silently ignored.

**Affected shapes** (all silently mis-return — verified empirically):

- Objectives (originally discovered):
  `MAXIMIZE MIN(expr) WHEN <never-true>` and the MAX / MIN-hard / MAX-hard
  mirrors. Covered by `test/decide/tests/test_edge_cases.py::test_maximize_min_objective_when_empty`
  and three siblings — currently the tests accept the meaningless value.

- Non-composed constraint-side mirrors:
  - `(MAX(x * v) WHEN <never-true>) >= K` — silently returns `OPTIMAL`
  - `(MIN(x * v) WHEN <never-true>) <= K` — silently returns `OPTIMAL`

- Composed constraints (any direction, when the MIN/MAX term has empty
  WHEN): the per-term auxiliary `z_k` has no pinning constraints and
  floats free, which vacates the **entire** additive constraint — not
  just the MIN/MAX contribution. Example:
  - Baseline `SUM(x * v) <= 8` on `v = [10, 5, 7]` → x = [0, 0, 1]
    (SUM = 7, respects bound).
  - Composed `SUM(x * v) + (MAX(x * v) WHEN FALSE) <= 8` on the same
    data → x = [1, 1, 1] (SUM = 22, violates the bound). The constraint
    is silently a no-op.
  - Affects easy-direction composed too (`SUM + MAX <= K`), because
    composed always routes through the `z_k` auxiliary path per
    `03_expressivity/such_that/done.md:82` — there is no
    "strip-the-aggregate" shortcut for composed shapes the way there is
    for non-composed easy direction.

- Non-composed easy-direction (`(MAX(x * v) WHEN FALSE) <= K`,
  `(MIN(x * v) WHEN FALSE) >= K`) is **not** affected — those paths emit
  per-row constraints only, and an empty row set produces zero
  constraints (vacuously true matches `-∞ <= K` / `+∞ >= K`, which is
  mathematically correct).

- Composed hard-direction (`SUM + MAX >= K`) is currently rejected at
  bind time by the v1 guard, so the empty-WHEN behavior there is
  unobservable today; the same `z_k` root cause would apply once the v1
  restriction is lifted.

**Decision needed**: silent-skip (current) vs. infeasibility vs. bind-time
rejection. The root `todo.md` already directs "Reject all cases of an
empty set," which covers both the objective and constraint flavors in one
sweep. Touches the flat-objective path at `physical_decide.cpp:3413-3498`
and the composed/constraint linking paths in the same file. Not urgent —
user queries that hit this almost certainly indicate a `WHEN` typo — but
the bug is broader than the objective-only framing the docs previously
carried.

**Status**: bug is now pinned via `@pytest.mark.xfail(strict=True)` in
`test/decide/tests/test_edge_cases.py`:

- The four objective-side tests (`test_maximize_min_objective_when_empty`
  and three siblings at lines ~599-791) were converted from
  documenting-only `try/except + print` to xfail-strict with
  `assert_error(sql, match=r"infeasible|empty|WHEN|MIN|MAX")`.
- Four constraint-side mirrors added: `test_max_when_empty_constraint_hard`,
  `test_min_when_empty_constraint_hard`,
  `test_sum_plus_max_when_empty_silently_vacates_constraint`, and the
  non-xfail regression pin `test_composed_easy_min_when_empty_regression_pin`
  (which captures the coincidental current OPTIMAL-with-x=[1,1] behavior
  for `SUM + MIN(WHEN FALSE) >= K`).

When the empty-set rejection lands, the xfail marks flip to XPASS-strict
failures, forcing the fix-author to update the marks. The regression pin
will likely also fail and need to be either deleted or converted to
`assert_error` like its siblings.

## Cross-references

- `PER + hard MIN/MAX` — see also `per/done.md`
- `entity_scope + hard MIN/MAX` — see also `entity_scope/done.md`
- Composed MIN/MAX expressivity — see `03_expressivity/such_that/done.md`
  and `03_expressivity/maximize_minimize/done.md`
