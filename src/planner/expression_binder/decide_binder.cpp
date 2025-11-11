#include "duckdb/planner/expression_binder/decide_binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
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

static bool IsVariableExpressionConst(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	const auto &colref = expr.Cast<const ColumnRefExpression>();
	if (colref.IsQualified()) {
		return false;
	}
	return variables.count(colref.GetColumnName()) > 0;
}

static idx_t CountDecideVariableOccurrencesInternal(const ParsedExpression &expr,
                                                    const case_insensitive_map_t<idx_t> &variables) {
	idx_t count = 0;
	if (IsVariableExpressionConst(expr, variables)) {
		count++;
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		count += CountDecideVariableOccurrencesInternal(child, variables);
	});
	return count;
}

bool ExpressionContainsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	return CountDecideVariableOccurrencesInternal(expr, variables) > 0;
}

static bool ValidateSumArgumentInternal(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                        bool &has_decide_variable, string &error_msg) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
        if (IsVariableExpression(expr, variables)) {
            has_decide_variable = true;
        }
        return true;
    }
    case ExpressionClass::CONSTANT:
        return true;
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		string func_name_lower = StringUtil::Lower(func.function_name);
		if (!func.is_operator) {
			if (func_name_lower == "sum") {
				error_msg = "Nested SUM() inside DECIDE SUM expression is not supported";
			} else {
				error_msg = StringUtil::Format("Unsupported function '%s' inside DECIDE SUM expression", func.function_name);
			}
			return false;
		}
		if (func_name_lower == "*" || func_name_lower == "+") {
			for (auto &child : func.children) {
				if (!ValidateSumArgumentInternal(*child, variables, has_decide_variable, error_msg)) {
					return false;
				}
			}
			if (func_name_lower == "*") {
				idx_t decide_count = CountDecideVariableOccurrencesInternal(expr, variables);
				if (decide_count > 1) {
					error_msg = StringUtil::Format("SUM expression must remain linear in DECIDE variables; found '%s'", expr.ToString());
					return false;
				}
			}
			return true;
		}
		if (func_name_lower == "-") {
			error_msg = "DECIDE SUM expression should not contain '-' operators; rewrite using explicit negative factors";
			return false;
		}
		error_msg = StringUtil::Format("Unsupported operator '%s' inside DECIDE SUM expression", func.function_name);
		return false;
	}
	case ExpressionClass::OPERATOR: {
		error_msg = StringUtil::Format("Unexpected operator expression inside DECIDE SUM expression: %s", expr.ToString());
		return false;
	}
	case ExpressionClass::CAST: {
		auto &cast = expr.Cast<CastExpression>();
		return ValidateSumArgumentInternal(*cast.child, variables, has_decide_variable, error_msg);
	}
    default:
        error_msg = StringUtil::Format("Unsupported expression of type ExpressionClass::%s inside DECIDE SUM expression",
                                       EnumUtil::ToString(expr.GetExpressionClass()));
        return false;
    }
}

bool ValidateSumArgument(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables, string &error_msg) {
	bool has_decide_variable = false;
	if (!ValidateSumArgumentInternal(expr, variables, has_decide_variable, error_msg)) {
		return false;
	}
	if (!has_decide_variable) {
		error_msg = "SUM expression must reference at least one DECIDE variable";
		return false;
	}
	DebugPrintParsed("ValidateSumArgument.ok", expr);
	return true;
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
