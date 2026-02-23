# DECIQL Expressivity Reference

This folder documents the expressive power of the DECIQL language — the SQL extension at the heart of PackDB. Each keyword/construct is a **subfolder** containing:

- `done.md` — What is implemented today, with syntax, examples, and code pointers
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

---

## Folders

| Folder | done.md covers | todo.md covers |
|---|---|---|
| [decide/](decide/) | IS BOOLEAN, IS INTEGER, multiple vars, scope, linearity | IS REAL variables |
| [such_that/](such_that/) | Comparisons, BETWEEN, IN, AND, subqueries, WHEN | Correlated subqueries, PER integration |
| [maximize_minimize/](maximize_minimize/) | SUM, multi-var, column arithmetic, WHEN on objective | ABS, PER on objective, COUNT/AVG |
| [when/](when/) | Full implementation (constraints + objectives) | Interaction with PER |
| [per/](per/) | *(nothing implemented)* | Full PER design + implementation plan |
| [sql_functions/](sql_functions/) | SUM, arithmetic, comparisons, BETWEEN, IN, NULL | ABS, COUNT, AVG, MIN/MAX linearization |

---

## Keyword Status Matrix

| Keyword / Feature | Implemented | Todo File |
|---|---|---|
| `DECIDE x IS BOOLEAN` | Yes | — |
| `DECIDE x IS INTEGER` | Yes | — |
| `DECIDE x IS REAL` | **No** | [decide/todo.md](decide/todo.md) |
| `DECIDE x` (default INTEGER) | Yes | — |
| Multiple variables: `DECIDE x, y` | Yes | — |
| `SUCH THAT` with `=`, `<>`, `<`, `<=`, `>`, `>=` | Yes | — |
| `AND` constraint separator | Yes | — |
| `BETWEEN ... AND ...` | Yes | — |
| `IN (...)` | Yes | — |
| Uncorrelated scalar subqueries | Yes | — |
| Correlated subqueries | **No** | [such_that/todo.md](such_that/todo.md) |
| Linear constraints | Yes | — |
| Non-linear constraints (`x * y`) | Not supported (by design) | — |
| `WHEN` on constraints | Yes | — |
| `WHEN` on objective | Yes | — |
| `PER` on constraints | **No** | [per/todo.md](per/todo.md) |
| `PER` on objective | **No** | [per/todo.md](per/todo.md) |
| `MAXIMIZE SUM(...)` | Yes | — |
| `MINIMIZE SUM(...)` | Yes | — |
| `SUM()` over decision variables | Yes | — |
| `COUNT()` over decision variables | **No** | [sql_functions/todo.md](sql_functions/todo.md) |
| `AVG()` over decision variables | **No** | [sql_functions/todo.md](sql_functions/todo.md) |
| `ABS()` | **No** | [sql_functions/todo.md](sql_functions/todo.md) |
| `MIN()` / `MAX()` over dec. vars | **No** | [sql_functions/todo.md](sql_functions/todo.md) |

---

## Development Priorities

1. **PER keyword** — enables per-group constraints (most impactful expressivity gain)
2. **COUNT/AVG aggregates** — syntactic convenience, linearizable for common cases
3. **IS REAL variables** — unlocks imputation, repair, and synthesis tasks
4. **ABS()** — needed for repair objectives (depends on IS REAL + Big-M)

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
