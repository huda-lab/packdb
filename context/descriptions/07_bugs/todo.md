# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

---

## System Catalog Access Breaks With `Parser Error: syntax error at or near "then"`

**Priority: Medium** (user DECIDE queries unaffected; blocks metadata introspection and any tool that scans the catalog)

### Symptom

Any query that causes DuckDB's built-in default views to be materialized fails at bind time with:

```
Parser Error: syntax error at or near "then"
```

The caret in the error points into the *outer* query text even though that text contains no `then` token — the error is coming from the lazy parse of a default-view SQL body during catalog binding.

### Reproduction

```sql
SELECT * FROM duckdb_tables();          -- FAIL
SELECT * FROM duckdb_views();           -- FAIL
SELECT * FROM sqlite_master;            -- FAIL
SELECT * FROM information_schema.tables;-- FAIL
SELECT * FROM pg_catalog.pg_class;      -- FAIL
SHOW TABLES;                            -- FAIL (rewrites to sqlite_schema query)
.tables                                 -- FAIL (same)

SELECT 1 FROM duckdb_tables() LIMIT 0;  -- OK (LIMIT 0 short-circuits before bind)
SELECT * FROM duckdb_schemas();         -- OK (does not trigger default-view scan)
SELECT * FROM duckdb_indexes();         -- OK
```

User DECIDE queries and regular user-table queries are unaffected.

### What is known

- The parser error message shows the outer query text, but the offending token is inside one of the default-view bodies registered in `src/catalog/default/default_views.cpp`. Those bodies are parsed lazily by `CreateViewInfo::FromSelect` the first time a scan walks over them.
- The trigger is any path that calls `Catalog::GetAllSchemas` + `schema.Scan(CatalogType::TABLE_ENTRY, ...)` with execution (not `LIMIT 0`), because that walk materializes default view catalog entries.
- Default-view SQL bodies that fail to parse when fed directly to the parser: `sqlite_master`, `duckdb_tables`, `duckdb_views`, `duckdb_columns`, `duckdb_constraints`, `pg_attribute`, `pg_attrdef`, `pg_class`, `pg_constraint`, `pg_description`, `pg_proc`, `pg_tables`, `pg_type`, `pg_views`, `information_schema.columns`, `information_schema.tables`, `referential_constraints`, `key_column_usage`, and more. (Many of these fail transitively because they reference `duckdb_tables` / `duckdb_views` views, which themselves fail.)
- Simplified standalone snippets extracted from the view bodies — e.g. `SELECT CASE WHEN temporary THEN 't' ELSE 'p' END relpersistence FROM (VALUES (true)) v(temporary);` — parse fine. The failure only manifests in the full view body, suggesting a token-level interaction specific to a construct inside one of the longer bodies rather than `CASE WHEN ... THEN` in isolation.

### Ruled out

- **`STRICT` keyword**: fully removed by commit `8395945b08` (diff on `third_party/libpg_query/grammar/keywords/func_name_keywords.list` shows `-STRICT_P`). No `STRICT`/`STRICT_P` remains in any keyword list.
- **Stale compiled parser**: `make grammar` produced a zero-byte diff against the committed `third_party/libpg_query/src_backend_parser_gram.cpp`. The grammar sources and compiled parser are already in sync.
- **`type` as a bareword column reference**: works fine (`SELECT 'x' AS type`, `SELECT type FROM (VALUES (...)) v(type)`).
- **`sql` as a column reference**: works (e.g. `SELECT sql FROM (VALUES ('x')) v(sql)`).
- **`CASE WHEN ... THEN ... END alias`** in isolation: works.

### Where to look next

1. Binary-search one of the failing view bodies (start with `pg_class` or `information_schema.tables` in `src/catalog/default/default_views.cpp`) to isolate the minimal sub-expression that fails under the current grammar.
2. Once the construct is identified, check `third_party/libpg_query/grammar/statements/select.y` for rules added during PackDB development that may shadow or conflict with it. Prime suspects:
   - The postfix `a_expr WHEN b_expr` / `a_expr WHEN b_expr PER columnref` rules added for DECIDE constraints/objectives (`select.y` around lines 232–335 and 2914).
   - Anything that altered operator precedence for `%prec POSTFIXOP` in combination with `WHEN`/`PER`.
3. The "near 'then'" token name in the error is a Bison artifact: the offending bareword is being lexed into a token that shares a yacc rule with `THEN`. That's the same signature as the `type`/`COUNT` class of keyword-bucket mistakes seen in prior commits (e.g. `5375579` "removed COUNT from packdb keywords").

### Impact

- Blocks `SHOW TABLES`, `DESCRIBE`, `.tables`, `information_schema.*`, `pg_catalog.*`, `sqlite_master`, any ORM/tool that introspects schema via catalog views.
- Does **not** block user DECIDE queries or normal user-table SELECTs. The stress-test plan can proceed using a user-created schema that does not rely on catalog introspection.

---

## Correlated Subquery Multiplied By Decision Variable On Both Sides → INTERNAL Error + Connection Invalidation

**Priority: High** (silently kills the DuckDB connection — every subsequent query in the same session fails with FATAL "database has been invalidated")

### Symptom

```
INTERNAL Error: Failed to add constraint to Gurobi: Problem adding constraints
```

After this fires, every subsequent query against the same connection raises:

```
FATAL Error: Failed: database has been invalidated because of a previous fatal error.
The database must be restarted prior to being used again.
```

So a single bad query in the middle of a session kills all the queries after it. Running `stress_queries/01_constraints.sql` end-to-end reproduces 15 cascading FATAL errors after C14.

### Reproduction (`stress_queries/01_constraints.sql` C14)

```sql
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT s_acctbal * pick >= (
    SELECT MIN(s2.s_acctbal) FROM supplier s2 WHERE s2.s_nationkey = supplier.s_nationkey
  ) * pick
  AND SUM(pick) >= 5
MAXIMIZE SUM(pick);
```

Trigger shape: per-row constraint where the same decision variable appears on both sides, and one side is a **correlated** scalar subquery scaled by that variable.

### What is known

- Simpler shapes (correlated subquery without the variable on both sides; uncorrelated subqueries) work fine. The trigger is the combination.
- The error message originates from `src/packdb/gurobi/gurobi_solver.cpp` after `api.addconstr` returns non-zero — i.e. PackDB has built a constraint structure that Gurobi rejects at submission time, rather than catching the issue at bind/optimizer time.

### Where to look next

1. Identify what the optimizer produces for the per-row LHS/RHS in this case (check `decide_optimizer.cpp` rewrites for correlated subquery + bilinear-like shapes).
2. The two issues are independent and both need addressing:
   - **Reject earlier.** The bind/optimizer pipeline should catch unsupported shapes and raise `InvalidInputException` before ever calling Gurobi.
   - **Don't invalidate the connection.** When Gurobi's `addconstr` returns an error, throwing `InternalException` from inside the physical operator marks the DuckDB connection fatal. Convert to `InvalidInputException` (or equivalent non-fatal type) at the throw site so the session survives a bad query.

### Impact

- Real correctness gap: a stress run on `01_constraints.sql` truncates at C14 and silently skips C15–C28.
- Same connection-invalidation pattern recurs in the bug below (R18).

---

## `POWER(AVG(qty), 2)` In Objective → INTERNAL Error + Connection Invalidation

**Priority: High** (same connection-invalidation cascade as the bug above)

### Symptom

```
INTERNAL Error: DECIDE objective contains a non-aggregate term: power(sum(qty), CAST(2 AS DOUBLE))
```

(`AVG` is rewritten to `SUM` upstream, so the eventual error talks about `power(sum(qty), 2)`.)

After this fires, the same `database has been invalidated` cascade applies. Running `stress_queries/05_rejected.sql` end-to-end produces 7 cascading FATAL errors after R18.

### Reproduction (`stress_queries/05_rejected.sql` R18)

```sql
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10
MAXIMIZE POWER(AVG(qty), 2);
```

The user wrapped an aggregate inside `POWER(_, 2)` instead of using the supported `SUM(POWER(_, 2))` shape. Should be rejected at bind time.

### What is known

- Other syntactically-similar non-aggregate-in-objective cases are caught cleanly at bind time with `Invalid Input` errors. `POWER(AVG(...), 2)` slips past the bind-time check because `AVG` is rewritten to `SUM` *after* the non-aggregate-term check sees the original form.
- The error then surfaces from later execution code as an `InternalException`, which is what poisons the connection.

### Where to look next

1. The bind-time rejection of "non-aggregate term in objective" should run **after** AVG-to-SUM rewriting, not before — so that the rewritten shape `POWER(SUM(qty), 2)` is what the check sees, and gets rejected with the existing clean error.
2. As with the bug above, the throw site that produces this `INTERNAL Error` should raise `InvalidInputException` instead so the connection survives a bad query.

### Impact

- Truncates `05_rejected.sql` at R18 and silently skips R19–R25.
- Combined with the C14 cascade above, suggests a general policy issue: **any `InternalException` thrown from inside DECIDE physical execution invalidates the DuckDB session**. A fix to the throw type alone (without addressing the bind-time root causes) would already turn these from fatal-cascading into single-query rejections.

