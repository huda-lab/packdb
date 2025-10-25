//===----------------------------------------------------------------------===//
//                         PackDB
//
// packdb/symbolic/decide_symbolic.hpp
//
// Symbolic Translation Layer for DECIDE Expressions
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

// Forward declare Symbolic from SymbolicC++ (global namespace)
class Symbolic;

namespace duckdb {

//! Context for symbolic translation - tracks DECIDE variables and row-varying columns
struct SymbolicTranslationContext {
    //! Map of DECIDE variable names to their indices
    const case_insensitive_map_t<idx_t> &decide_variables;
    
    //! Constructor
    explicit SymbolicTranslationContext(const case_insensitive_map_t<idx_t> &vars) 
        : decide_variables(vars) {}
};

//! Converts a ParsedExpression to a Symbolic object (for programmatic use)
//! Throws InternalException if the expression cannot be converted
::Symbolic ToSymbolicObj(const ParsedExpression &expr, SymbolicTranslationContext &ctx);

//! Converts a Symbolic object back to a ParsedExpression (preferred)
unique_ptr<ParsedExpression> FromSymbolic(const ::Symbolic &symbolic, SymbolicTranslationContext &ctx);

//! Helper: Check if an expression is a DECIDE variable
bool IsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

//! Helper: Check if an expression is row-varying (contains column references that aren't DECIDE variables)
bool IsRowVarying(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &decide_variables);

//! Normalize constraints: factor numeric scalars from SUM products on LHS and
//! adjust RHS/scalar accordingly; recurse through AND conjunctions.
unique_ptr<ParsedExpression> NormalizeDecideConstraints(const ParsedExpression &expr,
                                                        const case_insensitive_map_t<idx_t> &decide_variables);

//! Normalize objective: rewrite SUM inner as x * (row_expr) and combine numeric
//! constants inside the inner product; does not change overall scaling.
unique_ptr<ParsedExpression> NormalizeDecideObjective(const ParsedExpression &expr,
                                                      const case_insensitive_map_t<idx_t> &decide_variables);

//! Produce a Graphviz DOT representation of a ParsedExpression tree
string ExpressionToDot(const ParsedExpression &expr);

} // namespace duckdb

