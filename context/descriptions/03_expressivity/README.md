# DECIQL Expressivity Reference

This folder documents the expressive power of the DECIQL language — the SQL extension at the heart of PackDB. Each keyword/construct is a **subfolder** containing:

- `done.md` — What is implemented today, with syntax, examples, and code pointers
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

---

## Folders

| Folder | done.md covers | todo.md covers |
|---|---|---|
| [decide/](decide/) | IS BOOLEAN, IS INTEGER, IS REAL, multiple vars, scope, linearity | *(no planned features)* |
| [such_that/](such_that/) | Comparisons (`=`,`<`,`<=`,`>`,`>=`,`<>`), BETWEEN, IN (columns + dec. vars), AND, subqueries, WHEN, PER | Correlated subqueries |
| [maximize_minimize/](maximize_minimize/) | SUM, AVG, MIN/MAX, COUNT, multi-var, column arithmetic, WHEN on objective, ABS | PER on objective |
| [when/](when/) | Full implementation (constraints + objectives + PER composition) | *(no planned features)* |
| [per/](per/) | PER on constraints, WHEN+PER composition, row_group_ids architecture | Multi-column PER, PER on objective (partition-solve), row-varying RHS |
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
| Correlated subqueries | **No** | [such_that/todo.md](such_that/todo.md) |
| Linear constraints | Yes | — |
| Non-linear constraints (`x * y`) | Not supported (by design) | — |
| `WHEN` on constraints | Yes | — |
| `WHEN` on objective | Yes | — |
| `PER` on constraints | Yes | — |
| `PER` on objective | Accepted (no-op) | [per/todo.md](per/todo.md) (partition-solve) |
| `MAXIMIZE SUM(...)` | Yes | — |
| `MINIMIZE SUM(...)` | Yes | — |
| `SUM()` over decision variables | Yes | — |
| `COUNT()` over BOOLEAN variables | Yes (rewritten to SUM) | — |
| `COUNT()` over INTEGER variables | Yes (Big-M indicator rewrite) | — |
| `AVG()` over decision variables | Yes (RHS scaling) | — |
| `ABS()` | Yes (linearized) | — |
| `MIN()` / `MAX()` over dec. vars | Yes (per-row / Big-M) | — |

---

## Development Priorities

All previously planned priorities are **done**:
1. ~~**IS REAL variables**~~ — done
2. ~~**AVG() aggregate**~~ — done (RHS scaling)
3. ~~**ABS()**~~ — done (linearized via auxiliary variables)
4. ~~**`<>` and `IN` on decision variables**~~ — done (Big-M / auxiliary binary indicators)
5. ~~**COUNT() for INTEGER**~~ — done (Big-M indicator rewrite)
6. ~~**MIN() / MAX()**~~ — done (per-row + Big-M linearization)

**Remaining**:
- **PER on objective** (partition-solve) — see [per/todo.md](per/todo.md)
- **Multi-column PER** — see [per/todo.md](per/todo.md)
- **Correlated subqueries** — see [such_that/todo.md](such_that/todo.md)

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
