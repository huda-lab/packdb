# DECIDE Clause — Implemented Features

The `DECIDE` clause declares **decision variables** of a COP query. Each variable gets a value assigned by the solver for every row in the input relation, and the assigned values appear as new columns in the query result.

---

## Syntax

```sql
DECIDE variable_name [IS type] [, variable_name2 [IS type2] ...]
```

Variables are declared as a comma-separated list with optional type annotations.

---

## Variable Types

### `IS BOOLEAN`

The variable takes values in {0, 1}. PackDB automatically adds lower bound (`x >= 0`) and upper bound (`x <= 1`) constraints.

```sql
DECIDE x IS BOOLEAN
```

### `IS INTEGER`

The variable takes non-negative integer values {0, 1, 2, ...}. This is the **default** if no type is specified.

```sql
DECIDE x IS INTEGER
DECIDE x              -- same as IS INTEGER
```

---

## Multiple Variables

Multiple decision variables may be declared in a single `DECIDE` clause, separated by commas. Each variable produces an additional output column.

```sql
DECIDE x IS BOOLEAN, y IS INTEGER
```

---

## Variable Scope

All declared variables are available in:
- `SUCH THAT` constraints
- `MAXIMIZE` / `MINIMIZE` objective
- The `SELECT` list (returned as output columns)

Variables are **per-row**: the solver assigns one value per input row for each variable.

---

## Linearity Constraint

All expressions involving decision variables must be linear:

| Expression | Status |
|---|---|
| `x * 5` — variable times constant | OK |
| `x * column` — variable times table column (constant per row) | OK |
| `x + y` — sum of variables | OK |
| `x * y` — variable times variable | **ERROR** (non-linear) |
| `x * x` — variable squared | **ERROR** (non-linear) |

---

## Use Cases by Variable Type

| Task Category | Typical Variable | Type |
|---|---|---|
| Subset selection (knapsack, sampling) | `keep` | `BOOLEAN` |
| Outlier removal | `keep` | `BOOLEAN` |
| Counterfactual explanation | `keepS`, `keepP` | `BOOLEAN` |
| Scheduling / assignment | `hours_assigned` | `INTEGER` |

---

## Examples

```sql
-- Basic boolean selection
DECIDE keep IS BOOLEAN

-- Integer variable with default type
DECIDE quantity

-- Multiple typed variables
DECIDE x IS BOOLEAN, y IS INTEGER
```

---

## Code Pointers

- **Grammar** (variable type rule): `third_party/libpg_query/grammar/statements/select.y:170-177`
  ```
  variable_type: INTEGER | REAL | BOOLEAN_P
  ```
  Note: REAL is parsed but rejected at bind time.

- **Grammar** (comma-separated variable list): `select.y:202-208`
  ```
  typed_decide_variable_list: typed_decide_variable | list ',' typed_decide_variable
  ```

- **Binder** (variable processing loop, type validation, REAL rejection):
  `src/planner/binder/query_node/bind_select_node.cpp:424-487`
  - Line 470-474: REAL type rejected with `BinderException`
  - Line 486: Boolean type detected via `type_marker == "bool_variable"`
