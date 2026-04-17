# ABS Linearization Test Coverage — Todo

## Closed

- **ABS in aggregate constraint with WHEN** — `test_abs_linearization.py::test_abs_constraint_aggregate_with_when` (2026-04-17). Oracle-compared test over lineitem with R-flagged returnflag. The `d_i` auxiliaries exist unconditionally (their linking constraints are row-independent), but only WHEN-matching rows contribute to the SUM. Designed so the aggregate bound is binding on the optimum — if the WHEN mask were dropped, PackDB would over-constrain and the oracle comparison would diverge.
- **PER + ABS aggregate constraint** — `test_per_interactions.py::test_per_abs_aggregate` (2026-04-15).

## Missing coverage

_(No known gaps in the ABS test surface at this time. Extend here when new
scenarios are surfaced by audits or bug reports.)_
