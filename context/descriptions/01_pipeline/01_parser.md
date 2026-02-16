# Parser & Symbolic Layer

## 1. Overview
The Parser and Symbolic Layer is the entry point for the `DECIDE` clause. Its primary responsibility is not just to build a parse tree, but to **normalize** the user's algebraic expressions into a canonical form that the system can optimize. This is a critical step because SQL allows flexible expression shapes (e.g., `x * 2 + 5`), whereas linear solvers require a strict `coeff * variable` structure.

**Key Source File**: `src/packdb/symbolic/decide_symbolic.cpp`

## 2. Symbolic Translation
PackDB integrates `SymbolicC++` to perform algebraic manipulations. The translation pipeline is as follows:

1.  **DuckDB to Symbolic**: The `ToSymbolicRecursive` function traverses the DuckDB `ParsedExpression` tree.
    -   `ColumnRef` (decision variable) $\rightarrow$ `Symbolic Variable`
    -   `ColumnRef` (normal column) $\rightarrow$ `Symbolic Constant` (treated as opaque for now)
    -   `Operator` (+, -, *) $\rightarrow$ `Symbolic Operation`

2.  **Normalization**: The symbolic engine simplifies the expression. This involves:
    -   Expanding parentheses: `2 * (x + 5)` $\rightarrow$ `2x + 10`
    -   Collecting like terms: `x + x` $\rightarrow$ `2x`
    -   Separating constants.

3.  **Symbolic to DuckDB**: The `FromSymbolic` function converts the simplified symbolic expression back into a DuckDB `ParsedExpression`, structured specifically for the Binder.

## 3. Canonical Forms

The parser ensures that all constraints and objectives are rewritten into the following canonical forms before they reach the Binder.

### 3.1 Constraints
All constraints are normalized to:
$$ \sum (c_i \cdot x_i) \leq K - \sum (RowTerm_j) $$
Where:
-   $x_i$: Decision variables.
-   $c_i$: Coefficients (can be expressions).
-   $K$: A constant.
-   $RowTerm_j$: Terms involving only table columns (no decision variables).

**Example Transformation**:
Input SQL:
```sql
SUM(profit * x - cost) <= 500
```
Internal Steps:
1.  Symbolic: $\sum (P \cdot x - C) \leq 500$
2.  Split Sum: $\sum (P \cdot x) - \sum C \leq 500$
3.  Rearrange: $\sum (P \cdot x) \leq 500 + \sum C$

The Parser rewrites the expression tree so that the LHS contains **only** decision-dependent terms, and the RHS contains **only** scalar terms.

### 3.2 Objectives
Objectives are similarly normalized to:
$$ \text{MAX/MIN } \sum (c_i \cdot x_i) $$
Constant offsets in the objective (e.g., `MAX SUM(x * p + 10)`) are dropped from the optimization problem as they do not affect the optimal choice of $x$, though they are technically preserved in the final projection if needed.

## 4. Interaction with Binder
The Binder receives this normalized tree. It no longer needs to perform algebraic rearrangement; it simply validates that the structure matches the expectation (linear sum on LHS, scalar on RHS) and binds the column references.

## 5. `WHEN` Keyword (Conditional Constraints)

The parser handles the `WHEN` postfix keyword for conditional constraints via a DECIDE-scoped grammar rule. WHEN is **not** added to the global `a_expr` production (which would conflict with `CASE expr WHEN`), but instead lives in a dedicated `decide_constraint_item` non-terminal:

```yacc
decide_constraint_item:
    a_expr WHEN a_expr    /* constraint WHEN condition */
    | a_expr              /* unconditional constraint */
    ;
```

The parser emits a `PG_AEXPR_WHEN_CONSTRAINT` node, which the transformer converts to a `FunctionExpression("__when_constraint__", [constraint, condition])`. The symbolic layer normalizes the constraint child while passing through the condition unchanged.
