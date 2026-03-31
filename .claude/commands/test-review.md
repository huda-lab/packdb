# /test-review — DECIDE Test Coverage Gap Finder

Systematically audits test coverage against the full DECIDE expressivity surface. Answers the question: "Are we testing everything a user might want to express?" Finds missing scenarios, untested feature interactions, and edge cases that could silently break.

## Arguments

`$ARGUMENTS` — optional scope:
- *(empty)* — full audit across all feature areas
- a feature name (e.g., `per`, `when`, `min_max`, `entity_scope`, `quadratic`) — audit only that area
- `interactions` — focus specifically on cross-feature interaction gaps (WHEN+PER, QP+WHEN, entity_scope+PER, etc.)
- `errors` — focus on error handling coverage (are all invalid inputs properly rejected?)

## Procedure

### 1. Parse arguments and determine scope

Parse `$ARGUMENTS` into a scope. If empty, audit everything.

### 2. Gather the expressivity surface

Read these docs to build the complete picture of what's expressible:

- `context/descriptions/03_expressivity/README.md` — feature index
- `context/descriptions/03_expressivity/decide/done.md` — variable types, table-scoped vars
- `context/descriptions/03_expressivity/such_that/done.md` — constraint operators, subqueries
- `context/descriptions/03_expressivity/when/done.md` — conditional constraints/objectives
- `context/descriptions/03_expressivity/per/done.md` — grouped constraints/objectives
- `context/descriptions/03_expressivity/maximize_minimize/done.md` — objective types, nesting, QP
- `context/descriptions/03_expressivity/sql_functions/done.md` — SUM, COUNT, AVG, MIN, MAX, ABS
- `context/descriptions/03_expressivity/problem_types/done.md` — LP, ILP, MILP, QP, MIQP
- `.claude/CLAUDE.md` — syntax reference (condensed spec)

Also read `context/descriptions/04_optimizer/rewrite_passes/done.md` to understand all algebraic rewrites — each rewrite is a behavior that needs testing.

If scoped to a specific feature, only read the relevant subset.

### 3. Gather current test coverage

Read the test files to understand what IS tested:

- `test/decide/pytest.ini` — markers (coverage categories)
- `test/decide/conftest.py` — fixtures and infrastructure
- All test files in `test/decide/tests/` — read each one to catalog every test case

For each test, extract:
- **What feature(s)** it exercises (variable type, constraint type, objective type, WHEN, PER, etc.)
- **What scenario** it covers (basic usage, edge case, error case, interaction)
- **What data shape** it uses (single row, many rows, multiple groups, empty groups, etc.)

Build a coverage matrix mentally — features on one axis, scenario types on the other.

### 4. Spawn parallel audit agents

Launch up to 3 agents in parallel.

---

**Agent 1 — Single-Feature Gap Finder**

> You are auditing PackDB's DECIDE test suite for **gaps in single-feature coverage**. For each DECIDE feature, check whether all documented behaviors are tested.
>
> **Read the expressivity docs** (listed above) to understand every feature's documented behavior. Then **read every test file** in `test/decide/tests/` to see what's actually tested. For each feature, check:
>
> 1. **Variable types**:
>    - IS BOOLEAN: tested with all constraint types? With objectives? With WHEN? With PER?
>    - IS INTEGER: same checks
>    - IS REAL: same checks. Are fractional solutions tested (not just integer-valued)?
>    - Multiple variables: tested with different types in the same query? (e.g., `DECIDE x IS BOOLEAN, y IS REAL`)
>    - Table-scoped variables (`DECIDE Table.var`): tested with all constraint types? With WHEN? With PER? Mixed with row-scoped vars?
>
> 2. **Constraint operators** (each of `=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`, `IN`):
>    - Tested on per-row constraints?
>    - Tested on aggregate (SUM) constraints?
>    - Tested with negative coefficients?
>    - Tested with zero RHS?
>    - `<>`: tested with the Big-M indicator rewrite?
>    - `IN`: tested on decision variables (not just columns)?
>
> 3. **Aggregates** (SUM, COUNT, AVG, MIN, MAX):
>    - Each aggregate tested in constraints AND objectives?
>    - COUNT: tested for BOOLEAN (→SUM rewrite) AND INTEGER (→Big-M indicator)?
>    - AVG: tested with uneven group sizes (where AVG ≠ SUM behavior)?
>    - MIN/MAX: all easy cases tested? All hard cases tested? Both constraint and objective usage?
>
> 4. **WHEN clause**:
>    - Tested on per-row constraints, aggregate constraints, and objectives separately?
>    - Compound conditions (AND, OR)?
>    - WHEN that filters ALL rows (empty result)?
>    - WHEN with string equality, numeric comparison, NULL handling?
>
> 5. **PER clause**:
>    - Single column, multi-column?
>    - PER on constraints with all aggregate types?
>    - PER on objectives with nested aggregates (SUM(SUM(...)), MIN(MAX(...)), etc.)?
>    - Groups of different sizes? Single-row groups? Empty groups after WHEN filtering?
>
> 6. **Quadratic Programming**:
>    - All three syntax forms: `POWER(expr, 2)`, `expr ** 2`, `(expr) * (expr)`?
>    - With WHEN? With PER? With multiple variables?
>    - MAXIMIZE rejection tested?
>    - Mixed linear + quadratic objectives?
>
> 7. **Error handling**:
>    - Every documented restriction/limitation has a test that verifies the error is raised?
>    - Nonlinear constraint (`x * y`) rejected?
>    - Invalid aggregate rejected?
>    - PER on flat MIN/MAX objective rejected?
>    - MAXIMIZE with quadratic rejected?
>
> **Scope**: {scope_description}
>
> **Output format**:
> ```
> ## Single-Feature Coverage Gaps
>
> ### [Feature Name]
> **Tested**: [summary of what IS tested]
> **Missing**:
> - [scenario not tested] — why it matters: [what could break undetected]
> - [scenario not tested] — why it matters: [what could break undetected]
> ```

---

**Agent 2 — Interaction Gap Finder**

> You are auditing PackDB's DECIDE test suite for **cross-feature interaction gaps**. Many bugs live at the intersection of two features. Your job is to find feature combinations that are NOT tested together.
>
> **Read the test files** in `test/decide/tests/` and **read the expressivity docs** to understand all features. Then systematically check the following interaction matrix:
>
> For each pair of features, check if there is at least one test that combines them:
>
> | Feature A | Feature B | What to look for |
> |-----------|-----------|------------------|
> | WHEN | PER | Conditional grouped constraint — does WHEN filter interact correctly with PER grouping? |
> | WHEN | MIN/MAX | Conditional MIN/MAX — does WHEN work on linearized MIN/MAX constraints? |
> | WHEN | COUNT | Conditional count — does WHEN interact with COUNT→SUM or COUNT→Big-M rewrites? |
> | WHEN | AVG | Conditional AVG — does WHEN interact with AVG→SUM scaling? |
> | WHEN | ABS | Conditional ABS — does WHEN work on linearized ABS constraints? |
> | WHEN | QP | Conditional quadratic objective |
> | WHEN | `<>` | Conditional not-equal — WHEN + Big-M disjunction |
> | WHEN | entity_scope | Conditional with table-scoped variables |
> | PER | MIN/MAX | Grouped MIN/MAX — nested aggregate linearization |
> | PER | COUNT | Grouped COUNT — does PER interact with rewritten SUM? |
> | PER | AVG | Grouped AVG — coefficient scaling per group size |
> | PER | ABS | Grouped ABS constraints |
> | PER | `<>` | Grouped not-equal constraints |
> | PER | entity_scope | Grouped constraints with table-scoped variables |
> | PER | QP | Grouped quadratic objective |
> | entity_scope | MIN/MAX | Table-scoped variables with MIN/MAX linearization |
> | entity_scope | COUNT | Table-scoped variables with COUNT rewrite |
> | entity_scope | AVG | Table-scoped variables with AVG |
> | entity_scope | multiple vars | Mixed table-scoped + row-scoped variables |
> | entity_scope | QP | Table-scoped variables with quadratic objectives |
> | QP | multiple vars | Quadratic with multiple decision variables |
> | multiple vars | PER | Multiple variables with grouped constraints |
> | multiple vars | WHEN | Multiple variables with conditional constraints |
> | BOOLEAN + INTEGER | same query | Mixed variable types in constraints/objectives |
> | BOOLEAN + REAL | same query | Mixed variable types |
> | subquery | WHEN | Subquery RHS with conditional constraint |
> | subquery | PER | Subquery RHS with grouped constraint |
> | JOIN | PER | JOIN source with grouped constraints |
> | JOIN | entity_scope | JOIN source with table-scoped variables |
> | JOIN | WHEN | JOIN source with conditional constraints |
>
> Also check for **triple interactions** (three features combined):
> - WHEN + PER + MIN/MAX
> - WHEN + PER + entity_scope
> - WHEN + PER + multiple variables
> - entity_scope + PER + WHEN
> - QP + WHEN + multiple variables
>
> **Scope**: {scope_description}
>
> **Output format**:
> ```
> ## Interaction Coverage Matrix
>
> ### TESTED (confirmed by existing test)
> | Feature A | Feature B | Test File | Test Name |
> |-----------|-----------|-----------|-----------|
>
> ### NOT TESTED (gap)
> | Feature A | Feature B | Risk | Example Query That Would Test It |
> |-----------|-----------|------|----------------------------------|
>
> ### TRIPLE INTERACTIONS
> | Features | Tested? | Risk |
> |----------|---------|------|
> ```

---

**Agent 3 — Edge Case & Data Shape Auditor**

> You are auditing PackDB's DECIDE test suite for **edge cases and unusual data shapes** that could expose bugs. Your focus is on boundary conditions, degenerate inputs, and realistic usage patterns that are easy to overlook.
>
> **Read the test files** in `test/decide/tests/` (especially `test_edge_cases.py`) and the expressivity docs. Then check if these scenarios are tested:
>
> **Boundary conditions**:
> - Zero rows matching (empty result after WHERE or WHEN filters everything)
> - Single row (trivial problem — should still work)
> - All decision variables forced to same value by constraints
> - Infeasible problem (constraints contradict each other)
> - Unbounded problem (no objective bounds)
> - All-zero coefficients in objective
> - Negative coefficients in constraints and objectives
> - Very large coefficients (numeric stability)
> - RHS = 0 for constraints
> - Variables appearing in constraints but NOT in objective
> - Variables appearing in objective but NOT in constraints (unconstrained)
>
> **Data shapes**:
> - Single group in PER (entire table is one group)
> - PER where one group has 1 row and another has 1000 rows (asymmetric)
> - WHEN that filters to exactly 1 row in a group
> - WHEN that filters out an entire PER group (group becomes empty)
> - NULL values in PER columns (group key is NULL)
> - NULL values in WHEN condition columns
> - NULL values in constraint coefficient columns
> - Duplicate values in PER columns (many rows per group)
> - JOIN that produces duplicate rows (fan-out)
>
> **Realistic stress patterns**:
> - Multiple WHEN conditions on different constraints in the same query
> - Multiple PER constraints with different grouping columns
> - Constraint where LHS and RHS both contain decision variables
> - Objective with many terms (10+ columns multiplied by decision variable)
> - Query with 5+ constraints of mixed types (per-row + aggregate + PER)
> - Subquery that returns exactly one row vs multiple rows
> - Correlated subquery referencing decision variable (should this work? is it tested either way?)
>
> **Solver-specific**:
> - Are there tests that verify identical results between Gurobi and HiGHS? Or does the oracle only use one solver?
> - QP with HiGHS (continuous only) — is the restriction tested/enforced?
>
> **Scope**: {scope_description}
>
> **Output format**:
> ```
> ## Edge Case & Data Shape Audit
>
> ### TESTED (found in test suite)
> - [scenario] — tested in [test_file::test_name]
>
> ### NOT TESTED (gap)
> - [scenario] — risk: [what could break] — example: [brief SQL or description]
>
> ### UNCLEAR (couldn't determine)
> - [scenario] — [why it's unclear]
> ```

---

### 5. Synthesize findings

Once all three agents return, merge their results:

1. **Collect** all gaps from all agents
2. **Deduplicate**: if two agents flag the same missing scenario, merge
3. **Prioritize** by risk:
   - **HIGH** — Missing test for a rewrite or linearization (silent math errors), a feature interaction that touches the optimizer (rewrites can interfere), or a boundary condition that could crash
   - **MEDIUM** — Missing test for a documented feature used standalone, or an error case that should be rejected
   - **LOW** — Missing edge case that's unlikely in practice, or a style/organization issue
4. **Group** by feature area for easy navigation

### 6. Present the report

```
# Test Coverage Gap Report
**Scope**: [full / feature name] | **Date**: [date]
**Tests Audited**: [count] across [file count] files
**Coverage Surface**: [brief description of what was checked against]

## Summary
[2-3 sentences: overall coverage health, biggest gap areas, estimated number of new tests needed]

## High-Risk Gaps ([count])
These gaps could hide silent correctness bugs — prioritize first.

| # | Gap | Feature(s) | Risk | Example Query |
|---|-----|------------|------|---------------|
| 1 | ... | ...        | ...  | ```sql ...``` |

## Medium-Risk Gaps ([count])
Documented features or error cases without coverage.

| # | Gap | Feature(s) | Risk |
|---|-----|------------|------|

## Low-Risk Gaps ([count])
Edge cases and unlikely scenarios.

| # | Gap | Feature(s) |
|---|-----|------------|

## Interaction Matrix Summary
[Which feature pairs are well-tested, which have zero coverage]

## Cross-Cutting Observations
[Patterns — e.g., "entity_scope has almost no interaction tests", "QP is only tested standalone"]

## Recommended New Tests (prioritized)
1. **[test name]** — Tests [what]. Add to `test/decide/tests/[file]`.
   ```sql
   -- Example query this test should run
   SELECT ... FROM ... DECIDE ... SUCH THAT ... MINIMIZE ...
   ```
2. **[test name]** — Tests [what].
   ```sql
   ...
   ```
3. ...
```

### 7. Offer to write tests

After presenting the report, ask: "Want me to write these tests? I'll start with the high-risk gaps and work down."

If the user says yes, write the tests following the existing pattern:
- Use the oracle comparison pattern from existing tests (PackDB subprocess → oracle solver → compare)
- Follow naming conventions: `test_<feature>_<scenario>`
- Add appropriate pytest markers
- Place in the correct test file (or create a new one if a new category)
