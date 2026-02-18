#include "duckdb/planner/expression_binder/decide_objective_binder.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

namespace duckdb {

DecideObjectiveBinder::DecideObjectiveBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables)
    : DecideBinder(binder, context, variables){
}

BindResult DecideObjectiveBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	if (binding_when_condition) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	auto &expr = *expr_ptr;
    // DebugPrintParsed("BindObjective.input", expr);
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
	    auto &func = expr.Cast<FunctionExpression>();
	    // PackDB: Handle WHEN on objective: MAXIMIZE SUM(...) WHEN condition
	    if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
	        D_ASSERT(func.children.size() == 2);
	        // Validate: WHEN condition cannot reference DECIDE variables
	        if (ExpressionContainsDecideVariable(*func.children[1], variables)) {
	            return BindResult(BinderException::Unsupported(*expr_ptr,
	                "WHEN conditions in MAXIMIZE/MINIMIZE cannot reference DECIDE variables. "
	                "The WHEN condition must only reference table columns."));
	        }
	        // Bind the objective (child[0]) through normal objective binding
	        is_top_expression = true;
	        ErrorData obj_error;
	        BindChild(func.children[0], depth, obj_error);
	        if (obj_error.HasError()) {
	            return BindResult(std::move(obj_error));
	        }
	        // Bind the condition (child[1]) using base ExpressionBinder
	        is_top_expression = false;
	        binding_when_condition = true;
	        ErrorData cond_error;
	        try {
	            BindChild(func.children[1], depth, cond_error);
	        } catch (...) {
	            binding_when_condition = false;
	            throw;
	        }
	        binding_when_condition = false;
	        if (cond_error.HasError()) {
	            return BindResult(std::move(cond_error));
	        }
	        // Construct tagged BoundConjunctionExpression
	        auto &bound_objective = BoundExpression::GetExpression(*func.children[0]);
	        auto &bound_condition = BoundExpression::GetExpression(*func.children[1]);
	        auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	        result->children.push_back(std::move(bound_objective));
	        result->children.push_back(BoundCastExpression::AddCastToType(context, std::move(bound_condition), LogicalType::BOOLEAN));
	        result->alias = WHEN_CONSTRAINT_TAG;
	        return BindResult(std::move(result));
	    }
	    // DebugPrintParsed("BindObjective.input", expr);
	        if (is_top_expression && GetExpressionType(expr, error_msg) == DecideExpression::INVALID) {
	            return BindResult(BinderException::Unsupported(expr, error_msg));
	        }
	        is_top_expression = false;
	        auto result = BindFunction(expr_ptr, depth);
            if (result.HasError()) {
                return result;
            }
            return result;
	}
    case ExpressionClass::SUBQUERY:
        return DecideBinder::BindExpression(expr_ptr, depth, root_expression);
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
            if (!ValidateSumArgument(*func.children.front(), variables, error_msg)) {
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
