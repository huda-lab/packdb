# Bilinear Term Test Coverage — Todo

All previously documented gaps are now closed. See `done.md` for the
covered scenarios and the cross-feature interaction tests at the bottom
of `test/decide/tests/test_bilinear.py`.

## Closed in this session (2026-04-15)

- HIGH: PER + bilinear → `test_bilinear_per_group`
- HIGH: bilinear + WHEN + PER triple → `test_bilinear_when_per_triple`
- HIGH: entity_scope + bilinear → `test_bilinear_entity_scoped`
- MEDIUM: bilinear in MINIMIZE objective → `test_bilinear_minimize_objective`
- LOW: McCormick feasibility for Bool × Real in constraint → `test_bilinear_bool_real_constraint`

While writing `test_bilinear_minimize_objective` we found and fixed a
real bug: `MINIMIZE SUM(coeff * b * x)` (left-associative parse
`(coeff*b)*x`) was silently dropping `coeff` from the objective when the
optimizer rewrote the bilinear term to a McCormick auxiliary. MAXIMIZE
masked the bug because the corner solution is symmetric. Fix: extract
multiplicative coefficients from both factors (unwrapping CASTs) and
fold them into the auxiliary replacement. See
`src/optimizer/decide/decide_optimizer.cpp::ExtractMultiplicativeCoefficient`
and the call sites in `FindAndReplaceBilinear`.

## Future candidates (LOW)

- Triple bilinear chain `(b1 * x) * (b2 * y)` — currently rejected as
  multi-variable expression on a McCormick side. Verify the rejection
  message is actionable.
- Bool × Bool with data coefficient in the AND-linearization branch
  (e.g., `coeff * b1 * b2` MINIMIZE) — the same fix path covers it, but
  no dedicated regression test yet.
