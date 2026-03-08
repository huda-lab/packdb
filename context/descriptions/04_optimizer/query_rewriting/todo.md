# Query Rewriting — Planned Features

**This is the primary near-term optimizer focus area.**

---

## Big-M Reformulation

**Priority: High** (needed to support ABS, MIN, MAX linearization)

Big-M is a standard ILP technique to linearize conditional and disjunctive constraints by introducing binary indicator variables and a large constant M.

### Core Idea

To enforce "if condition then constraint":
- Introduce binary variable `z`
- Replace constraint `x <= K` with `x <= K + M * (1 - z)`
- When `z = 1`, the constraint is enforced; when `z = 0`, it's relaxed by M

The constant M must be chosen large enough that the relaxed constraint is never binding, but small enough to maintain numerical stability.

### Applications in PackDB

1. **ABS linearization**: `ABS(expr)` becomes auxiliary variable `d` with:
   - `d >= expr`
   - `d >= -expr`
   (This specific case doesn't need Big-M, just auxiliary variables. But more complex conditional objectives do.)

2. **MIN/MAX linearization**: `z = MAX(x_1, ..., x_n)` requires Big-M constraints linking z to each x_i via indicator binaries.

3. **Conditional constraints** (future): "if x > 0 then y >= K" style constraints require Big-M to model the implication.

### Implementation Approach

Add a **rewrite pass** between the binder and physical execution that:
1. Detects ABS/MIN/MAX in constraint/objective expressions
2. Introduces auxiliary variables (extending the decision variable list)
3. Generates the linearized constraint set
4. Computes appropriate M values from data bounds (scan the input relation for min/max of relevant columns)

**Key code touchpoints**:
- `logical_decide.cpp` — extend to carry auxiliary variables
- `physical_decide.cpp` — extend matrix construction for auxiliary constraints
- New file: `src/packdb/optimizer/big_m_rewriter.cpp` (suggested)

---

## Constraint Push-Down

**Priority: High**

Push constraint evaluation closer to the data access layer to reduce the amount of data materialized before solving.

### Idea

If a constraint can be partially evaluated during scanning (before the full relation is materialized), we can:
1. Eliminate rows that cannot participate in any feasible solution
2. Reduce the size of the ILP matrix passed to the solver

### Example

```sql
DECIDE x IS BOOLEAN
SUCH THAT
    SUM(x * weight) <= 50 AND
    x <= 1 WHEN category = 'electronics'
```

The second constraint implies that non-electronics rows can have x > 1. But if x IS BOOLEAN, all rows already have x <= 1. The constraint is redundant and can be eliminated.

More powerfully: if we know `SUM(x * weight) <= 50` and the minimum weight in the data is 5, then at most 10 items can be selected. If we can identify which 10 items are "best" (via the objective), we can prune the rest.

### Relation to Skyband Indexing

This is closely related to the skyband pruning approach (see [../problem_reduction/todo.md](../problem_reduction/todo.md)). Push-down is the simpler version that doesn't require building an index — just applying logical deductions from constraints.

---

## Constraint Pull-Out

**Priority: Medium** (becomes important when PER is implemented)

Extract common sub-expressions from multiple constraints into shared intermediate results.

### Motivation

When `PER` generates many constraints (one per group), they often share the same coefficient structure with different row masks. Instead of computing coefficients independently for each generated constraint:

1. Compute the coefficient vector once
2. Apply group-specific masks to produce each constraint's coefficients

This is an execution optimization, not a logical rewrite.

### Example

```sql
SUM(x * weight) <= 40 PER empID
```

If there are 1000 employees, this generates 1000 constraints. All share the coefficient `weight` — only the row mask (which rows belong to which employee) differs. Pull-out computes `x * weight` once and masks it 1000 times.

---

## Constraint-to-Bound Conversion

**Priority: Medium**

Detect constraints that are equivalent to simple variable bounds and convert them to solver-native bounds rather than matrix rows.

### Example

```sql
SUCH THAT x <= 1    -- per-row constraint
```

This generates one matrix row per input row. But `x <= 1` is just an upper bound on the variable. Solvers handle bounds much more efficiently than matrix constraints (they're stored in a separate bounds vector, not in the constraint matrix).

### Detection Rules

- `x <= K` (constant) -> upper bound
- `x >= K` (constant) -> lower bound
- `x = K` (constant) -> fixed variable
- These are only convertible when the constraint applies to ALL rows (no WHEN/PER modifier)

### Implementation

Add a pass that scans constraints before matrix construction, identifies bound-equivalent ones, removes them from the constraint list, and sets bounds directly on the solver variables.

---

## Migrate Binder Rewrite Passes to Optimizer

**Priority: Medium** (cleanup / architecture improvement)

Several algebraic rewrite passes currently live in `bind_select_node.cpp` as part of the binder. These are not semantic validation — they are algebraic transformations that should live in a dedicated optimizer layer.

### Current Binder Rewrites (Proto-Optimizer)

1. **COUNT → SUM** (`RewriteCountToSum`, lines ~413-466):
   - BOOLEAN: `COUNT(x)` → `SUM(x)` (direct)
   - INTEGER: Creates indicator variable + Big-M linking constraints
   - This is an algebraic rewrite, not validation

2. **AVG → SUM** (`RewriteAvgToSum`, lines ~490-505):
   - Tags the rewrite so RHS is scaled by row count at execution time
   - Pure algebraic transformation

3. **ABS Linearization** (`RewriteAbsLinearization`, lines ~510-591):
   - Creates auxiliary REAL variable + 2 linearization constraints
   - Standard LP technique, not validation logic

### Target Architecture

These should become rules in a `LogicalDecideOptimizer` that runs after binding but before physical planning. DuckDB's optimizer framework uses rules registered in `src/optimizer/optimizer.cpp` — a `DecideOptimizer` would follow the same pattern:

```
Binder → LogicalDecide → DecideOptimizer (rewrite rules) → PhysicalDecide
```

### Benefits of Migration

- **Reduced DuckDB core footprint**: `bind_select_node.cpp` is a core DuckDB file; moving rewrites to `src/packdb/` reduces merge conflicts on version upgrades.
- **Composability**: New rewrites (MIN/MAX linearization, conditional constraints) can be added as independent rules.
- **Testability**: Individual rewrite rules can be unit-tested in isolation.

### Implementation Steps

1. Create `src/packdb/optimizer/decide_rewrite.cpp` + header
2. Move `RewriteCountToSum`, `RewriteAvgToSum`, `RewriteAbsLinearization` to new file
3. Call from a `DecideOptimizer` pass or as a post-bind hook
4. Update `bind_select_node.cpp` to import and call the extracted functions

### Expression Analysis Migration (Optional, Longer-Term)

The expression analysis functions in `physical_decide.cpp` (lines 28-254) — `AnalyzeConstraint()`, `AnalyzeObjective()`, `TraverseBoundsConstraints()`, `ExtractVariableBounds()` — could also move to logical optimization. This would allow the physical layer to focus solely on coefficient evaluation, and would enable future optimizer rules like bound tightening to operate on the analyzed constraint structure.

---

## Bound Tightening

**Priority: Low** (future optimization, requires expression analysis migration first)

Once variable bounds are extracted at the logical level, an optimizer pass could:
1. Derive tighter bounds from constraint interactions (e.g., `SUM(x) <= 5` with 10 BOOLEAN variables implies at most 5 can be 1)
2. Fix variables that are forced by constraints (e.g., `x >= 1` and `x <= 1` → fix `x = 1`)
3. Pass tighter bounds to the solver for faster solving
