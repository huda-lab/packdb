#include "duckdb/planner/expression_binder/decide_objective_binder.hpp"

namespace duckdb {

DecideObjectiveBinder::DecideObjectiveBinder(Binder &binder, ClientContext &context, const case_insensitive_set_t &variables)
    : DecideBinder(binder, context, variables){
}

BindResult DecideObjectiveBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
    string error_msg;
	switch (expr.GetExpressionClass()) {
    case ExpressionClass::COLUMN_REF:
    case ExpressionClass::CONSTANT: {
        if (!is_top_expression) {
            return ExpressionBinder::BindExpression(expr_ptr, depth);
        }
        break;
    }
    case ExpressionClass::FUNCTION: {
        if (is_top_expression && GetExpressionType(expr, error_msg) == DecideExpression::INVALID) {
            return BindResult(BinderException::Unsupported(expr, error_msg));
        }
        is_top_expression = false;
        return BindFunction(expr_ptr, depth);
	}
	default:
        break;
	}
    return BindResult(BinderException::Unsupported(expr, StringUtil::Format("[MAXIMIZE|MINIMIZE] clause does not support '%s'(ExpressionClass::%s)", expr.ToString(), EnumUtil::ToString(expr.GetExpressionClass()))));
}

DecideExpression DecideObjectiveBinder::GetExpressionType(ParsedExpression &expr, string& error_msg){
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		if (StringUtil::Lower(func.function_name) == "sum") {
            if (!ValidateSumArgument(*func.children.front(), variables, error_msg, true)) {
                error_msg += ", found '" + expr.ToString() + "'";
                return DecideExpression::INVALID;
            }
            return DecideExpression::SUM;
		} else {
            error_msg = StringUtil::Format("[MAXIMIZE|MINIMIZE] clause does not support function '%s', only SUM is allowed.", func.function_name);
            return DecideExpression::INVALID;
        }
    }
    default: {
        error_msg = StringUtil::Format("The objective of the [MAXIMIZE|MINIMIZE] clause must be a SUM expression over a DECIDE variable (e.g., SUM(x * a) / SUM(x)). Found '%s' instead.", expr.ToString());
    	return DecideExpression::INVALID;
    }
    }
}

} // namespace duckdb