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

namespace duckdb {

bool IsScalarValue(ParsedExpression &expr);

bool IsVariableExpression(ParsedExpression &expr, const case_insensitive_set_t &variables);

bool HasVariableExpression(ParsedExpression &expr, const case_insensitive_set_t &variables);

bool ValidateSumArgument(ParsedExpression &expr, const case_insensitive_set_t &variables, string &error_msg, bool top_argument);

//! The DecideBinder is a base class for binders in DECIDE statements
class DecideBinder : public ExpressionBinder {
public:
    DecideBinder(Binder &binder, ClientContext &context, const case_insensitive_set_t &variables);

protected:
    BindResult BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth);
    BindResult BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
    virtual DecideExpression GetExpressionType(ParsedExpression &expr, string &error_msg) {
        throw duckdb::NotImplementedException("GetExpressionType is not implemented for this binder.");
    }

    bool is_top_expression;
    case_insensitive_set_t variables;
};

} // namespace duckdb