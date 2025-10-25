# Step 1: Symbolic Translation Layer - Detailed Implementation Guide

## 🎯 Goal
Build translation functions between DuckDB's `ParsedExpression` and SymbolicC++'s `Symbolic` objects to enable symbolic manipulation of DECIDE expressions.

---

## 📁 Files Created

1. **Header**: `/src/packdb/symbolic/decide_symbolic.hpp`
2. **Implementation**: `/src/packdb/symbolic/decide_symbolic.cpp`
3. **Build**: `/src/packdb/symbolic/CMakeLists.txt`
4. **Test**: `/test/packdb/test_symbolic_translation.test` (to be created)

---

## 🔧 Implementation Strategy: Incremental with Debug Statements

### Phase 1: Helper Functions ✅ (COMPLETED)

**Files Updated:**
- `decide_symbolic.cpp` - `IsDecideVariable()` and `IsRowVarying()`

**What We Did:**
1. Implemented `IsDecideVariable()` - checks if a `ParsedExpression` is a DECIDE variable
2. Implemented `IsRowVarying()` - checks if expression contains row-varying columns
3. Added debug statements using the `deb()` macro

**How to Test:**
```bash
# Build the project
cd /home/hqr9411/working_code/packdb
make test_symbolic_simple
```

---

### Phase 2: ToSymbolic - Basic Types ✅ (COMPLETED)

**What We Did:**
1. Implemented `CONSTANT` → `Symbolic(number)`
2. Implemented `COLUMN_REF` → `Symbolic(symbol_name)`
3. Added extensive debug output

**Test Cases to Verify:**
- `5` → `Symbolic(5.0)`
- `3.14` → `Symbolic(3.14)`
- Column `x` → `Symbolic("x")`
- Column `calories` → `Symbolic("calories")`

---

### Phase 3: ToSymbolic - Operators ✅ (COMPLETED)

**What We Did:**
1. Implemented binary operators:
   - `+` → `Sum`
   - `-` → `Subtract`
   - `*` → `Product`
   - `/` → `Division`
2. Recursive conversion of children

**Test Cases to Verify:**
- `2 + 3` → `Symbolic(2) + Symbolic(3)` → expands to `5`
- `x * 5` → `Symbolic("x") * Symbolic(5)`
- `x + y` → `Symbolic("x") + Symbolic("y")`
- `2*x + 3*y` → complex symbolic expression

---

### Phase 4: ToSymbolic - Functions (IN PROGRESS)

**What We Did:**
1. Special handling for `SUM()` aggregate
2. Created marker: `__SUM__` symbol multiplied by inner expression
   - Example: `SUM(x*calories)` → `Symbolic("__SUM__") * (Symbolic("x") * Symbolic("calories"))`

**Why This Approach?**
- SymbolicC++ doesn't have built-in aggregate functions
- We need to distinguish `SUM(x+y)` (aggregate) from `x+y` (regular expression)
- The marker allows us to reconstruct the aggregate in `FromSymbolic`

**Test Cases to Verify:**
- `SUM(x)` → `__SUM__ * x`
- `SUM(2*x)` → `__SUM__ * (2*x)`
- `SUM(x*calories + y*protein)` → `__SUM__ * (x*calories + y*protein)`

**Next Steps:**
- Need to handle edge cases (empty SUM, nested SUMs)
- May need different markers for different aggregate types in future

---

### Phase 5: FromSymbolic - Reverse Conversion (TODO)

**Objective:** Convert simplified `Symbolic` back to `ParsedExpression`

**Strategy:**
1. Check the type of `Symbolic` using `symbolic.type()`
2. Use `CastPtr<T>` to access specific types:
   - `CastPtr<const Numeric>` for numbers
   - `CastPtr<const Symbol>` for variables
   - `CastPtr<const Sum>` for sums
   - `CastPtr<const Product>` for products

**Implementation Pattern:**
```cpp
unique_ptr<ParsedExpression> FromSymbolic(const Symbolic &symbolic, SymbolicTranslationContext &ctx) {
    deb("\n=== FromSymbolic ===");
    deb("Input symbolic:", symbolic);
    deb("Type:", symbolic.type().name());
    
    // Check if it's a number
    if (symbolic.type() == typeid(Numeric)) {
        CastPtr<const Numeric> num(symbolic);
        // Extract value and create ConstantExpression
        double value = /* extract from num */;
        return make_uniq<ConstantExpression>(Value::DOUBLE(value));
    }
    
    // Check if it's a symbol
    if (symbolic.type() == typeid(Symbol)) {
        CastPtr<const Symbol> sym(symbolic);
        string name = /* extract symbol name */;
        
        // Special handling for __SUM__ marker
        if (name == "__SUM__") {
            // Need to handle specially
        }
        
        return make_uniq<ColumnRefExpression>(name);
    }
    
    // Check if it's a sum
    if (symbolic.type() == typeid(Sum)) {
        CastPtr<const Sum> sum(symbolic);
        
        // Iterate through summands
        for (auto &term : sum->summands) {
            // Recursively convert each term
        }
        
        // Build operator expression with ADD
    }
    
    // Check if it's a product
    if (symbolic.type() == typeid(Product)) {
        CastPtr<const Product> prod(symbolic);
        
        // Check for __SUM__ marker in factors
        bool has_sum_marker = false;
        for (auto &factor : prod->factors) {
            // Check if factor is __SUM__
        }
        
        if (has_sum_marker) {
            // Build FunctionExpression for SUM
        } else {
            // Build regular MULTIPLY operator
        }
    }
    
    throw InternalException("FromSymbolic: Unsupported symbolic type");
}
```

**Test Cases:**
1. Simple round-trip: `5` → `Symbolic(5)` → `5`
2. Variable: `x` → `Symbolic("x")` → `x`
3. Addition: `x + y` → `Symbolic` → `x + y`
4. Complex: `2*x + 3*y` → `Symbolic` → `2*x + 3*y`
5. Aggregate: `SUM(x*calories)` → `Symbolic` → `SUM(x*calories)`

---

## 🧪 Testing Approach

### Method 1: Unit Test File

Create `/test/packdb/test_symbolic_translation.test`:

```sql
# name: test/packdb/test_symbolic_translation
# description: Test symbolic translation layer
# group: [packdb]

# This test will verify the symbolic translation works
# by checking that complex expressions can be normalized

statement ok
ATTACH 'build/packdb.db' AS packdb;

statement ok
SET search_path = 'packdb,main';

# Simple expression that should trigger ToSymbolic
statement ok
SELECT * FROM lineitem
DECIDE x
SUCH THAT SUM(x) = 10
MAXIMIZE SUM(x*l_quantity)
LIMIT 1;
```

### Method 2: Standalone C++ Test Program

Create `/test/packdb/test_symbolic_standalone.cpp`:

```cpp
#include "duckdb/packdb/symbolic/decide_symbolic.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include <iostream>

using namespace duckdb;

int main() {
    std::cout << "=== Symbolic Translation Tests ===\n\n";
    
    // Test 1: Simple constant
    {
        std::cout << "Test 1: Constant\n";
        auto expr = make_uniq<ConstantExpression>(Value::INTEGER(42));
        
        case_insensitive_map_t<idx_t> vars;
        SymbolicTranslationContext ctx(vars);
        
        auto symbolic = ToSymbolic(*expr, ctx);
        std::cout << "Result: " << symbolic << "\n\n";
    }
    
    // Test 2: Column reference (DECIDE variable)
    {
        std::cout << "Test 2: DECIDE Variable\n";
        auto expr = make_uniq<ColumnRefExpression>("x");
        
        case_insensitive_map_t<idx_t> vars;
        vars["x"] = 0;
        SymbolicTranslationContext ctx(vars);
        
        auto symbolic = ToSymbolic(*expr, ctx);
        std::cout << "Result: " << symbolic << "\n\n";
    }
    
    // Test 3: Simple addition
    {
        std::cout << "Test 3: Addition (2 + 3)\n";
        auto left = make_uniq<ConstantExpression>(Value::INTEGER(2));
        auto right = make_uniq<ConstantExpression>(Value::INTEGER(3));
        
        auto expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_ADD, 
            std::move(left), std::move(right));
        
        case_insensitive_map_t<idx_t> vars;
        SymbolicTranslationContext ctx(vars);
        
        auto symbolic = ToSymbolic(*expr, ctx);
        std::cout << "Result: " << symbolic << "\n\n";
    }
    
    // Test 4: Variable multiplication
    {
        std::cout << "Test 4: Multiplication (2 * x)\n";
        auto left = make_uniq<ConstantExpression>(Value::INTEGER(2));
        auto right = make_uniq<ColumnRefExpression>("x");
        
        auto expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_MULTIPLY,
            std::move(left), std::move(right));
        
        case_insensitive_map_t<idx_t> vars;
        vars["x"] = 0;
        SymbolicTranslationContext ctx(vars);
        
        auto symbolic = ToSymbolic(*expr, ctx);
        std::cout << "Result: " << symbolic << "\n\n";
    }
    
    std::cout << "=== All Tests Complete ===\n";
    return 0;
}
```

---

## 🚀 How to Build and Test Incrementally

### Step 1: Build the library
```bash
cd /home/hqr9411/working_code/packdb
make
```

### Step 2: Check for compilation errors
```bash
# If there are errors, they'll show up during make
# Fix any include issues or syntax errors
```

### Step 3: Run DuckDB with debug output
```bash
# The debug statements will print to console when you run queries
./build/release/duckdb build/packdb.db

# Try a simple DECIDE query to trigger the translation
```

### Step 4: Add temporary debug code
In `bind_select_node.cpp`, around line 430, add test code:

```cpp
if (statement.HasDecideClause()) {
    // TEST SYMBOLIC TRANSLATION
    deb("\n=== TESTING SYMBOLIC TRANSLATION ===");
    
    // Create a simple test expression: 2 * x
    auto const_2 = make_uniq<ConstantExpression>(Value::INTEGER(2));
    auto var_x = make_uniq<ColumnRefExpression>("x");
    auto mult_expr = make_uniq<OperatorExpression>(
        ExpressionType::OPERATOR_MULTIPLY,
        std::move(const_2),
        std::move(var_x)
    );
    
    SymbolicTranslationContext test_ctx(decide_variable_names);
    auto symbolic_result = ToSymbolic(*mult_expr, test_ctx);
    deb("ToSymbolic(2*x) =", symbolic_result);
    
    deb("=== END SYMBOLIC TRANSLATION TEST ===\n");
    
    // Continue with normal DECIDE binding...
}
```

---

## 📊 Expected Debug Output

When you run a DECIDE query, you should see output like:

```
=== ToSymbolic ===
Input expression: OPERATOR_MULTIPLY(2, x)
Expression class: OPERATOR

Processing OPERATOR: OPERATOR_MULTIPLY
Number of children: 2

  Converting child 0
=== ToSymbolic ===
Input expression: 2
Expression class: CONSTANT
Processing CONSTANT: 2
  -> Converted to numeric: 2

  Converting child 1
=== ToSymbolic ===
Input expression: x
Expression class: COLUMN_REF
Processing COLUMN_REF: x
IsDecideVariable: Checking expression: x
  -> Column name: x Is DECIDE variable: true
  -> This is a DECIDE variable, creating symbol: x

  -> Creating Product
Result: 2*x
```

---

## ✅ Success Criteria for Phase 5 (FromSymbolic)

1. **Round-trip works**: `ParsedExpression` → `Symbolic` → `ParsedExpression` produces equivalent expressions
2. **Constants preserved**: `5` → `5`
3. **Variables preserved**: `x` → `x`
4. **Operations preserved**: `2*x + 3*y` → `2*x + 3*y`
5. **Aggregates reconstructed**: `SUM(x*calories)` → `SUM(x*calories)`

---

## 🐛 Common Issues and Debugging

### Issue 1: Compilation errors with SymbolicC++
**Solution:** Check includes and make sure SymbolicC++ headers are in include path

### Issue 2: Segfault when accessing Symbolic
**Solution:** Use `symbolic.type()` to check type before casting

### Issue 3: Can't extract value from Numeric
**Solution:** Look at SymbolicC++ docs for proper Numeric API

### Issue 4: Round-trip produces different expression
**Solution:** Add more debug output in FromSymbolic to see where conversion diverges

---

## 📝 Next Steps After Completing Step 1

Once `ToSymbolic` and `FromSymbolic` are working:

1. **Step 2**: Implement normalization functions that use symbolic manipulation
2. **Step 3**: Integrate into binder (call normalizer before DecideConstraintsBinder)
3. **Step 4**: Update binder validation rules
4. **Step 5**: Update execution layer
5. **Step 6**: Comprehensive end-to-end tests

---

## 🔗 Useful References

- DuckDB Expression Classes: `/src/include/duckdb/parser/expression/*.hpp`
- SymbolicC++ Documentation: `third_party/symboliccpp/symbolic/symbolicc++.h`
- Example usage: `bind_select_node.cpp` lines 431-456
- Debug utility: `src/packdb/utility/debug.cpp`

