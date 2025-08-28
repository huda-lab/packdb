#include "duckdb/planner/expression_binder/decide_binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"

#include "duckdb/packdb/utility/debug.hpp"

namespace duckdb {

bool IsScalarValue(ParsedExpression &expr) {
    if (expr.GetExpressionClass() == ExpressionClass::CONSTANT) {
        auto &constant_expr = expr.Cast<ConstantExpression>();
        switch (constant_expr.value.type().id()) {
            case LogicalTypeId::LIST:
            case LogicalTypeId::STRUCT:
            case LogicalTypeId::MAP:
            case LogicalTypeId::ARRAY:
            case LogicalTypeId::UNION:
                return false;
            default:
                return true;
        }
    }
    if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
        auto &subquery_expr = expr.Cast<SubqueryExpression>();
        if (subquery_expr.subquery_type == SubqueryType::SCALAR) {
            return true;
        }
    }
    return false;
}

bool IsVariableExpression(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
    if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF){
        auto &colref = expr.Cast<ColumnRefExpression>();
        if (!colref.IsQualified()) {
            return variables.count(colref.GetColumnName()) > 0;
        }
    }
    return false;
}

bool HasVariableExpression(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
    // Base case: Check if the current expression itself is a variable.
    if (IsVariableExpression(expr, variables)) {
        return true;
    }

    // Special case for subqueries.
    if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
        auto &subquery_expr = expr.Cast<SubqueryExpression>();

        // 1. Check the comparison child of the subquery (e.g., the 'x' in 'x > ANY(...)').
        if (subquery_expr.child && HasVariableExpression(*subquery_expr.child, variables)) {
            return true;
        }

        // 2. Recursively check the QueryNode of the subquery itself.
        if (subquery_expr.subquery && subquery_expr.subquery->node) {
            bool found_in_subquery = false;
            // Use EnumerateQueryNodeChildren to traverse SELECT, WHERE, HAVING, etc.
            ParsedExpressionIterator::EnumerateQueryNodeChildren(
                *subquery_expr.subquery->node,
                [&](unique_ptr<ParsedExpression> &child) {
                    if (HasVariableExpression(*child, variables)) {
                        found_in_subquery = true;
                    }
                },
                [&](TableRef &ref) {}
            );
            if (found_in_subquery) {
                return true;
            }
        }
    }

    // General recursive step for all other expression types.
    bool has_variable = false;
    ParsedExpressionIterator::EnumerateChildren(
        expr,
        [&](ParsedExpression &child) {
            if (HasVariableExpression(child, variables)) {
                has_variable = true;
            }
        }
    );

    return has_variable;
}

bool ValidateSumArgument(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables, string &error_msg, bool top_argument){
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
        if (IsVariableExpression(expr, variables)){
            if (!top_argument) {
                error_msg = "More than one occurrence of DECIDE variables in a SUM function";
    			return false;
            }
        } else {
            if (top_argument) {
                error_msg = "SUM function does not have DECIDE variables";
    			return false;
            }
        }
        return true;
	}
	case ExpressionClass::CONSTANT:
        return true;
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		if (top_argument){
            if (func.function_name != "*") {
    			error_msg = "Either SUM(x), SUM(f(a)*x) or SUM(x*f(a)) is allowed";
    			return false;
            }
            auto &left = *func.children.front();
            auto &right = *func.children.back();
            if (IsVariableExpression(left, variables)) {
                return ValidateSumArgument(right, variables, error_msg, false);
            } else {
                if (IsVariableExpression(right, variables)){
                    return ValidateSumArgument(left, variables, error_msg, false);
                } else {
                    error_msg = "SUM function does not have DECIDE variables as linear factors";
                    return false;
                }
            }
		} else {
            for (auto &child : func.children) {
                if (!ValidateSumArgument(*child, variables, error_msg, false)) {
                    return false;
                }
            }
            return true;
        }
	}
	default:
		// Any other expression type is invalid.
		error_msg = StringUtil::Format("Unsupported expression of type ExpressionClass::%s inside SUM()", EnumUtil::ToString(expr.GetExpressionClass()));
		return false;
	}
}

DecideBinder::DecideBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables) : ExpressionBinder(binder, context), variables(variables) {
    is_top_expression = true;        
}

BindResult DecideBinder::BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) {
	AggregateBinder aggregate_binder(binder, context);
	ErrorData error;

	// No Filter/Distinc allowed for Aggregate
	if (aggr.filter || aggr.distinct) {
        return BindResult(BinderException::Unsupported(aggr, StringUtil::Format("DECIDE clause does not support '%s'", aggr.ToString())));
	}

	// Bind arguments
	vector<unique_ptr<Expression>> children;
	vector<LogicalType> child_types;
	for (auto &child_expr : aggr.children) {
		aggregate_binder.BindChild(child_expr, 0, error);
		if (error.HasError()) {
			return BindResult(std::move(error));
		}
        auto &bound_child = BoundExpression::GetExpression(*child_expr);
		child_types.push_back(bound_child->return_type);
		children.push_back(std::move(bound_child));
	}

	// 2. Bind the aggregate function itself
	FunctionBinder function_binder(binder);
	auto best_function_idx = function_binder.BindFunction(func.name, func.functions, child_types, error);
	if (!best_function_idx.IsValid()) {
		error.AddQueryLocation(aggr);
		return BindResult(std::move(error));
	}
	auto bound_function = func.functions.GetFunctionByOffset(best_function_idx.GetIndex());

	// 3. Create the BoundAggregateExpression itself.
	auto bound_aggregate = function_binder.BindAggregateFunction(bound_function, std::move(children));
	// Note: We are NOT storing this aggregate in a BoundSelectNode. We are returning it directly.
	return BindResult(std::move(bound_aggregate));
}

BindResult DecideBinder::BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &function = expr.Cast<FunctionExpression>();
    // Check if this is an aggregate function
    QueryErrorContext error_context(expr_ptr->GetQueryLocation());
    auto &catalog_entry = *GetCatalogEntry(CatalogType::SCALAR_FUNCTION_ENTRY, function.catalog, function.schema, function.function_name, OnEntryNotFound::THROW_EXCEPTION, error_context);
    
    if (catalog_entry.type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
        // It's an aggregate function, bind it using our custom aggregate logic
        return BindAggregate(function, catalog_entry.Cast<AggregateFunctionCatalogEntry>(), depth);
    }
    // It's a scalar function, bind it normally
    return ExpressionBinder::BindExpression(expr_ptr, depth);
}

} // namespace duckdb