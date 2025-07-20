//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/decide_objective_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_binder/decide_binder.hpp"
#include "duckdb/common/enums/decide.hpp"

namespace duckdb {

//! The DecideObjective binder is responsible for binding an expression within the [MAXIMIZE|MINIMIZE] clause of a SQL statement
class DecideObjectiveBinder : public DecideBinder {
public:
    DecideObjectiveBinder(Binder &binder, ClientContext &context, const case_insensitive_set_t &variables);

protected:
    BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression = false) override;

private:
    DecideExpression GetExpressionType(ParsedExpression &expr, string &error_msg) override;
};

} // namespace duckdb