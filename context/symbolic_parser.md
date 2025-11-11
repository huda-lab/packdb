# Symbolic Normalization Overview

This note captures the essentials of PackDB's symbolic layer—the preprocessing step that turns DECIDE clause expressions into a canonical form before binding.

---

## 1. Why Symbolic Normalization?

- **Stable input for the binder:** Complex algebraic expressions are rewritten so that DECIDE-variable terms appear in a predictable structure.
- **Row/decide separation:** Any component that depends only on table columns (no DECIDE variables) is moved to the right-hand side of a comparison.
- **No coefficient rewriting:** Numeric factors stay attached to the original terms; we do not scale constraints.

Pipeline:

```
ParsedExpression
   └── ToSymbolic()     // convert to SymbolicC++
         └── simplify/expand
             └── FromSymbolic()  // back to ParsedExpression
```

---

## 2. Key Concepts

| Concept | Description |
| --- | --- |
| `SymbolicTranslationContext` | Carries DECIDE variable names so the converter can tell which symbols are decision variables. |
| `__SUM__` marker | Special symbolic factor used to remember that a sub-expression originated from a `SUM(...)`. |
| `CollectAdditiveTerms` | Recursively flattens a symbolic expression into additive pieces. |
| `ExtractDecideFactors` | Splits a symbolic term into DECIDE-variable factors and “other” factors. |

---

## 3. Constraint Normalization (Compare Expressions)

When we encounter `SUM(lhs_terms) < RHS`, we:

1. Convert the entire left expression to symbolic form.
2. Flatten additive terms and classify each term as:
   - **DECIDE term:** contains a DECIDE variable (after removing the `__SUM__` marker).
   - **Row term:** contains no DECIDE variables.
   - **Constant:** numeric literal.
3. Rebuild the left side as a single `SUM(...)` combining only DECIDE terms.
4. Accumulate constants into the RHS scalar.
5. For each row term, append `+ SUM(-row_term)` to the RHS.

Resulting structure:

```
SUM(decide_terms) <= rhs_constant + SUM(-row_term1) + … + SUM(-row_termN)
```

No factoring or GCD extraction is performed; coefficients remain untouched.

---

## 4. Objective Normalization

Objectives already arrive as `SUM(...)`. We convert the inner expression to symbolic form, drop any term that lacks a DECIDE variable, and rebuild the `SUM` so only decision-variable contributions remain.

---

## 5. Interaction with the Binder

The binder expects:

- **Constraints:** LHS must be a single `SUM(...)` containing DECIDE variables only; RHS may be a numeric constant plus any number of `SUM(...)` aggregates without DECIDE variables.
- **Objectives:** Single `SUM(...)` whose argument references at least one DECIDE variable. Multiple coefficients and mixed variables are now allowed.

See `context/binder_updates.md` for the binder-side adjustments and remaining work in the physical layer.

---

## 6. Current Limitations & Next Steps

- Equalities and BETWEEN clauses rely on the same logic but still have limited test coverage.
- Physical execution (`PhysicalDecide`) does not yet understand the richer bound expressions; it must be updated to analyze the new aggregate shapes.
- Additional validation (e.g., nested casts, unary minus) should be covered by tests to ensure both the symbolic layer and the binder accept them consistently.

This summary replaces the previous exhaustive guide; refer to the source (`src/packdb/symbolic/decide_symbolic.cpp`) for implementation details.
