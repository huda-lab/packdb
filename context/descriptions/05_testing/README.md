# Test Coverage Documentation

Tracks the state of the DECIDE test suite — what is covered, what gaps remain,
and which tests verify correctness (oracle comparison) vs. feasibility only.

## Structure

Each subdirectory corresponds to a feature area and contains:
- `done.md` — covered scenarios, with oracle vs. constraint-only status
- `todo.md` — remaining gaps and upgrade candidates

## Areas

| Directory | Feature | Status |
|-----------|---------|--------|
| `entity_scope/` | Table-scoped decision variables (`DECIDE Table.var`) | Active |

## How to use

When implementing a new feature or fixing a bug in a tested area:
1. Check the area's `done.md` to understand existing coverage.
2. After adding/changing tests, update `done.md` to reflect new state.
3. Move items from `todo.md` to `done.md` as they are completed.

## Coverage quality levels

- **oracle** — PackDB objective is compared against an independent reference solver. Catches silent optimality bugs (wrong coefficients, wrong constraint direction, rewrite errors).
- **constraint only** — Test checks that constraints are satisfied and the result is non-empty. Does not verify the solution is optimal. Can miss bugs where the solver produces a valid but suboptimal solution due to incorrect formulation.
- **xfail** — Feature not yet implemented or parser not yet working. Test is kept to auto-detect when the feature becomes available.
- **error test** — Verifies that an invalid query is rejected with the right error message.
