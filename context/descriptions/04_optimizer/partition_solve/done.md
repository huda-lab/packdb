# Partition-Solve — Done

Partition-solve is not yet implemented as an optimization, but the architectural foundation is in place.

## row_group_ids Foundation

The PER keyword implementation introduced a unified `row_group_ids` architecture (in `EvaluatedConstraint`) that assigns each row to a group. This is the key data structure that makes partition-solve detection straightforward:

| Case | row_group_ids | num_groups | ILP constraints |
|------|---------------|------------|-----------------|
| No WHEN, no PER | empty | 0 | 1 (all rows) |
| WHEN only | 0 or INVALID_INDEX | 1 | 1 (matching rows) |
| PER only | 0..K-1 | K | K (one per group) |
| WHEN + PER | 0..K-1 or INVALID_INDEX | K | K (filtered, grouped) |

When all constraints share the same `row_group_ids` mapping (same PER column, same K groups), the ILP is decomposable into K independent sub-problems. The `row_group_ids` vector directly provides the partition.

**Cross-reference**: See `context/descriptions/03_expressivity/per/done.md` for full PER implementation details and the `row_group_ids` architecture.
