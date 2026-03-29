#include "duckdb/planner/expression_binder/decide_binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"

#include "duckdb/packdb/utility/debug.hpp"
#include "duckdb/main/client_context.hpp"

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
	// Descend into subquery QueryNode bodies (SELECT, WHERE, HAVING, etc.)
	// EnumerateChildren only visits SubqueryExpression.child, not the query body.
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery_expr = expr.Cast<const SubqueryExpression>();
		if (subquery_expr.subquery && subquery_expr.subquery->node) {
			// const_cast: EnumerateQueryNodeChildren requires non-const but we only read
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    const_cast<QueryNode &>(*subquery_expr.subquery->node),
			    [&](unique_ptr<ParsedExpression> &child) {
				    count += CountDecideVariableOccurrencesInternal(*child, variables);
			    },
			    [&](TableRef &ref) {});
		}
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
			if (func_name_lower == "abs") {
				// ABS is allowed inside SUM — will be linearized by the optimizer.
				// Treat ABS as opaque: just verify it references a decide variable.
				if (func.children.size() != 1) {
					error_msg = "ABS requires exactly one argument";
					return false;
				}
				if (ExpressionContainsDecideVariable(*func.children[0], variables)) {
					has_decide_variable = true;
				}
				return true;
			}
			if (func_name_lower == "power" || func_name_lower == "pow") {
				// POWER(linear_expr, 2) — convex quadratic objective (QP).
				// Accept only when the exponent is exactly 2 and the base is
				// a linear expression in DECIDE variables.
				if (func.children.size() != 2) {
					error_msg = "POWER requires exactly two arguments";
					return false;
				}
				// Validate exponent is the constant 2
				auto &exponent = *func.children[1];
				if (exponent.GetExpressionClass() != ExpressionClass::CONSTANT) {
					error_msg = "POWER exponent in DECIDE expression must be a constant integer (only 2 is supported)";
					return false;
				}
				auto &exp_const = exponent.Cast<ConstantExpression>();
				double exp_val;
				try {
					exp_val = exp_const.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
				} catch (...) {
					error_msg = "POWER exponent must be numeric";
					return false;
				}
				if (exp_val != 2.0) {
					error_msg = StringUtil::Format(
					    "Only POWER(expr, 2) is supported for quadratic objectives. "
					    "Found exponent %g. Higher powers are not allowed.", exp_val);
					return false;
				}
				// Validate the base is a linear expression in DECIDE variables
				bool base_has_var = false;
				if (!ValidateSumArgumentInternal(*func.children[0], variables, base_has_var, error_msg)) {
					error_msg = "Inside POWER(..., 2): " + error_msg;
					return false;
				}
				if (!base_has_var) {
					error_msg = "POWER(expr, 2) in DECIDE objective must reference at least one DECIDE variable";
					return false;
				}
				has_decide_variable = true;
				return true;
			}
			if (func_name_lower == "min" || func_name_lower == "max" || func_name_lower == "sum" || func_name_lower == "avg") {
				// Nested aggregates (e.g., SUM(MAX(expr)) for PER objectives) are allowed.
				// The optimizer will detect and rewrite them.
				if (func.children.size() == 1 && ExpressionContainsDecideVariable(*func.children[0], variables)) {
					has_decide_variable = true;
					return true;
				}
				error_msg = StringUtil::Format("Nested %s() inside DECIDE expression must reference a DECIDE variable",
				                               StringUtil::Upper(func_name_lower));
				return false;
			}
			error_msg = StringUtil::Format("Unsupported function '%s' inside DECIDE SUM expression", func.function_name);
			return false;
		}
		if (func_name_lower == "**") {
			// ** is DuckDB's power operator — validate like POWER(expr, 2)
			if (func.children.size() != 2) {
				error_msg = "** operator requires exactly two arguments";
				return false;
			}
			auto &exponent = *func.children[1];
			if (exponent.GetExpressionClass() != ExpressionClass::CONSTANT) {
				error_msg = "POWER exponent in DECIDE expression must be a constant integer (only 2 is supported)";
				return false;
			}
			auto &exp_const = exponent.Cast<ConstantExpression>();
			double exp_val;
			try {
				exp_val = exp_const.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
			} catch (...) {
				error_msg = "POWER exponent must be numeric";
				return false;
			}
			if (exp_val != 2.0) {
				error_msg = StringUtil::Format(
				    "Only POWER(expr, 2) is supported for quadratic objectives. "
				    "Found exponent %g. Higher powers are not allowed.", exp_val);
				return false;
			}
			bool base_has_var = false;
			if (!ValidateSumArgumentInternal(*func.children[0], variables, base_has_var, error_msg)) {
				error_msg = "Inside ** 2: " + error_msg;
				return false;
			}
			if (!base_has_var) {
				error_msg = "** 2 in DECIDE objective must reference at least one DECIDE variable";
				return false;
			}
			has_decide_variable = true;
			return true;
		}
		if (func_name_lower == "-") {
			for (auto &child : func.children) {
				if (!ValidateSumArgumentInternal(*child, variables, has_decide_variable, error_msg)) {
					return false;
				}
			}
			return true;
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
					// Check if this is a squared expression: (expr) * (expr) where both sides are identical.
					// This is allowed as a QP pattern equivalent to POWER(expr, 2).
					if (func.children.size() == 2 &&
					    func.children[0]->ToString() == func.children[1]->ToString()) {
						has_decide_variable = true;
						return true;
					}
					error_msg = StringUtil::Format(
					    "Product of different DECIDE variable expressions is not supported. "
					    "For quadratic objectives, use POWER(expr, 2) or (expr) * (expr) "
					    "where both sides are the same linear expression. Found '%s'", expr.ToString());
					return false;
				}
			}
			return true;
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
    case ExpressionClass::SUBQUERY: {
        auto &subquery = expr.Cast<SubqueryExpression>();
        if (subquery.subquery_type != SubqueryType::SCALAR) {
            error_msg = "Subquery in DECIDE SUM expression must be scalar";
            return false;
        }
        if (ExpressionContainsDecideVariable(expr, variables)) {
            error_msg = "Subquery in DECIDE SUM expression cannot contain DECIDE variables";
            return false;
        }
        return true;
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
	// DebugPrintParsed("ValidateSumArgument.ok", expr);
	return true;
}

DecideBinder::DecideBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables) : ExpressionBinder(binder, context), variables(variables) {
    is_top_expression = true;        
}

BindResult DecideBinder::BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) {
	ErrorData error;

	// No Filter/Distinc allowed for Aggregate
	if (aggr.filter || aggr.distinct) {
        return BindResult(BinderException::Unsupported(aggr, StringUtil::Format("DECIDE clause does not support '%s'", aggr.ToString())));
	}

	// Bind arguments
	vector<unique_ptr<Expression>> children;
	vector<LogicalType> child_types;
	for (auto &child_expr : aggr.children) {
        // Use this->BindExpression to ensure subqueries are handled (executed at bind time)
        auto result = BindExpression(child_expr, depth);
        if (result.HasError()) {
            return result;
        }
        auto &bound_child = result.expression;
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

BindResult DecideBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
    if (depth > 0) {
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
    auto &expr = *expr_ptr;
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::FUNCTION:
        return BindFunction(expr_ptr, depth);
    case ExpressionClass::SUBQUERY: {
        auto &subquery = expr.Cast<SubqueryExpression>();
        if (subquery.subquery_type != SubqueryType::SCALAR) {
             return BindResult(BinderException::Unsupported(expr, "Only scalar subqueries are supported in DECIDE"));
        }
        if (ExpressionContainsDecideVariable(expr, variables)) {
            return BindResult(BinderException::Unsupported(expr,
                "Subqueries in DECIDE clauses cannot reference DECIDE variables."));
        }
        // Standard binding handles both correlated and uncorrelated scalar subqueries.
        // Uncorrelated: PlanSubqueries evaluates them as cross-joined scalars.
        // Correlated: PlanSubqueries decorrelates them into joins, producing
        //             per-row values that the DECIDE operator evaluates normally.
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
    default:
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
}



} // namespace duckdb
