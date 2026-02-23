# SUCH THAT Clause — Planned Features

## Correlated Subqueries

**Priority: Medium**

Currently only uncorrelated scalar subqueries are supported (evaluated once, treated as constant). Correlated subqueries reference the outer row:

```sql
-- NOT YET IMPLEMENTED
SUCH THAT
    x <= (SELECT budget FROM Depts WHERE Depts.id = item.dept_id)
```

### Why It's Blocked

Correlated subqueries require per-row evaluation that interacts with the solver logic in a way that currently breaks the matrix formulation pipeline. The subquery result would need to become a per-row coefficient.

### Suggested Approach

Implement a binder rule to **unnest** correlated subqueries into joins before the DECIDE clause is processed. This is a well-known technique in query optimization:

1. Detect correlated subqueries during binding
2. Rewrite them as left joins with the correlated table
3. The join result provides per-row constants that can be used as coefficients

---

## PER (Group-Scoped Constraints)

The `PER` keyword automatically generates one constraint per distinct value of a column. See [per/todo.md](../per/todo.md) for full design and implementation plan.

```sql
-- NOT YET IMPLEMENTED
SUCH THAT
    SUM(new_hours) <= 40 PER empID
```
