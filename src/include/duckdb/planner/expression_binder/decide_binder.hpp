//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/decide_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp" // Required for NotImplementedException
#include "duckdb/packdb/utility/debug.hpp"

namespace duckdb {

bool IsScalarValue(ParsedExpression &expr);

bool IsVariableExpression(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

bool ValidateSumArgument(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables, string &error_msg,
                         bool allow_quadratic = false);

bool ContainsQuadraticPattern(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

bool ExpressionContainsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);


// inline void DebugPrintParsed(const string &tag, const ParsedExpression &expr) {
// 	deb("[BINDER] ", tag, ": ", expr.ToString());
// }
// inline void DebugPrintBound(const string &tag, const Expression &expr) {
// 	deb("[BINDER] ", tag, ": ", expr.ToString());
// }

//! The DecideBinder is a base class for binders in DECIDE statements
class DecideBinder : public ExpressionBinder {
public:
    DecideBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables);

protected:
    BindResult BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) override;
    BindResult BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression = false) override;
    virtual DecideExpression GetExpressionType(ParsedExpression &expr, string &error_msg) {
        throw duckdb::NotImplementedException("GetExpressionType is not implemented for this binder.");
    }

    bool is_top_expression;
    case_insensitive_map_t<idx_t> variables;
};

} // namespace duckdb
