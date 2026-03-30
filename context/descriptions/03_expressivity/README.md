# DECIQL Expressivity Reference

This folder documents the expressive power of the DECIQL language — the SQL extension at the heart of PackDB. Each keyword/construct is a **subfolder** containing:

- `done.md` — What is implemented today, with syntax, examples, and code pointers
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

---

## Folders

| Folder | done.md covers | todo.md covers |
|---|---|---|
| [problem_types/](problem_types/) | LP, ILP, MILP, QP, MIQP, feasibility — problem class taxonomy, solver support matrix, structural properties | Negative domains, explicit bounds, QCQP, SOCP |
| [decide/](decide/) | IS BOOLEAN, IS INTEGER, IS REAL, multiple vars, scope, linearity | *(no planned features)* |
| [such_that/](such_that/) | Comparisons (`=`,`<`,`<=`,`>`,`>=`,`<>`), BETWEEN, IN (columns + dec. vars), AND, subqueries (uncorrelated + correlated), WHEN, PER | *(no planned features)* |
| [maximize_minimize/](maximize_minimize/) | SUM, multi-var, column arithmetic objectives; cross-refs to sql_functions, problem_types, when, per | *(no planned features)* |
| [when/](when/) | Full implementation (constraints + objectives + PER composition) | *(no planned features)* |
| [per/](per/) | PER on constraints (single + multi-column), PER on objective (nested aggregates), WHEN+PER composition, row_group_ids architecture | Row-varying RHS |
| [sql_functions/](sql_functions/) | SUM, COUNT (BOOLEAN/INTEGER), AVG, MIN/MAX, ABS, `<>`, IN (dec. vars), arithmetic, comparisons, BETWEEN, NULL | COUNT (REAL), division |

---

## Keyword Status Matrix

| Keyword / Feature | Implemented | Todo File |
|---|---|---|
| `DECIDE x IS BOOLEAN` | Yes | — |
| `DECIDE x IS INTEGER` | Yes | — |
| `DECIDE x IS REAL` | Yes | — |
| `DECIDE x` (default INTEGER) | Yes | — |
| Multiple variables: `DECIDE x, y` | Yes | — |
| `SUCH THAT` with `=`, `<`, `<=`, `>`, `>=` | Yes | — |
| `<>` (not-equal) | Yes (Big-M disjunction) | — |
| `AND` constraint separator | Yes | — |
| `BETWEEN ... AND ...` | Yes | — |
| `IN (...)` on table columns | Yes | — |
| `IN (...)` on decision variables | Yes (auxiliary binary indicators) | — |
| Uncorrelated scalar subqueries | Yes | — |
| Correlated scalar subqueries | Yes (per-row constraints; aggregate requires scalar RHS) | — |
| Linear constraints | Yes | — |
| Quadratic objective: `MINIMIZE SUM(POWER(expr, 2))` | Yes (convex QP, syntax-enforced) | — |
| Non-linear constraints (`x * y`) | Not supported (by design) | — |
| `WHEN` on constraints | Yes | — |
| `WHEN` on objective | Yes | — |
| `PER` on constraints | Yes | — |
| `PER` on objective | Yes (nested aggregate syntax) | — |
| `MAXIMIZE SUM(...)` | Yes | — |
| `MINIMIZE SUM(...)` | Yes | — |
| `SUM()` over decision variables | Yes | — |
| `COUNT()` over BOOLEAN variables | Yes (rewritten to SUM) | — |
| `COUNT()` over INTEGER variables | Yes (Big-M indicator rewrite) | — |
| `AVG()` over decision variables | Yes (RHS scaling) | — |
| `ABS()` | Yes (linearized) | — |
| `MIN()` / `MAX()` over dec. vars | Yes (per-row / Big-M) | — |

### Problem Classification

For a complete taxonomy of what mathematical optimization problem classes PackDB can express (LP, ILP, MILP, QP, MIQP, feasibility), see [problem_types/done.md](problem_types/done.md).

---

## Development Priorities

All previously planned priorities are **done**:
1. ~~**IS REAL variables**~~ — done
2. ~~**AVG() aggregate**~~ — done (RHS scaling)
3. ~~**ABS()**~~ — done (linearized via auxiliary variables)
4. ~~**`<>` and `IN` on decision variables**~~ — done (Big-M / auxiliary binary indicators)
5. ~~**COUNT() for INTEGER**~~ — done (Big-M indicator rewrite)
6. ~~**MIN() / MAX()**~~ — done (per-row + Big-M linearization)

7. ~~**Multi-column PER**~~ — done (`PER (col1, col2, ...)` with composite keys)

8. ~~**PER on objective**~~ — done (nested aggregate syntax with two-level auxiliary formulation)
9. ~~**Correlated subqueries**~~ — done (delegated to DuckDB's standard decorrelation; per-row constraints + scalar-RHS aggregate constraints)

**Remaining**:
- **Row-varying RHS with PER** — see [per/todo.md](per/todo.md)

---

## Background

DECIQL extends SQL with constrained optimization. The key structure:

```sql
SELECT select_list
FROM table_expression
[WHERE ...]
DECIDE variable_name [IS type] [, ...]
SUCH THAT
    constraint [AND constraint ...]
[MAXIMIZE | MINIMIZE] objective_expression
```

See `context/descriptions/00_project_overview/syntax_reference.md` for the full implemented syntax reference.
