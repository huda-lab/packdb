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
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/operator/helper/physical_materialized_collector.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/prepared_statement_data.hpp"

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
    auto &expr = *expr_ptr;
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::FUNCTION:
        return BindFunction(expr_ptr, depth);
    case ExpressionClass::SUBQUERY: {
        auto &subquery = expr.Cast<SubqueryExpression>();
        if (subquery.subquery_type != SubqueryType::SCALAR) {
             return BindResult(BinderException::Unsupported(expr, "Only scalar subqueries are supported in DECIDE"));
        }
        
        string subquery_sql = expr.ToString();
        string sql = "SELECT " + subquery_sql;
        
        // Manual execution to avoid deadlock (context.Query acquires lock)
        Parser parser;
        parser.ParseQuery(sql);
        if (parser.statements.empty()) {
             return BindResult(BinderException::Unsupported(expr, "Failed to parse subquery SQL"));
        }
        
        Planner planner(context);
        planner.CreatePlan(std::move(parser.statements[0]));

        // Check for correlated subquery - not supported in DECIDE clauses
        if (!planner.binder->correlated_columns.empty()) {
            return BindResult(BinderException::Unsupported(expr,
                "Correlated subqueries are not supported in DECIDE clauses. "
                "The subquery references columns from outer queries, which cannot be "
                "evaluated at optimization time. Please use a constant value, "
                "non-correlated subquery, or table column instead."));
        }

        Optimizer optimizer(*planner.binder, context);
        auto optimized_plan = optimizer.Optimize(std::move(planner.plan));
        
        PhysicalPlanGenerator generator(context);
        auto physical_plan = generator.CreatePlan(std::move(optimized_plan));
        
        // Wrap in result collector
        PreparedStatementData data(StatementType::SELECT_STATEMENT);
        data.types = physical_plan->GetTypes();
        for(size_t i=0; i<data.types.size(); ++i) data.names.push_back("col" + to_string(i));
        data.plan = std::move(physical_plan);
        
        auto collector = make_uniq<PhysicalMaterializedCollector>(data, false);
        // collector->children.push_back(std::move(physical_plan)); // Do not add as child, it uses data.plan reference
        
        Executor executor(context);
        executor.Initialize(std::move(collector));
        
        auto result = executor.ExecuteTask();
        while (result != PendingExecutionResult::EXECUTION_FINISHED && result != PendingExecutionResult::EXECUTION_ERROR) {
             result = executor.ExecuteTask();
        }
        
        if (executor.HasError()) {
             return BindResult(BinderException::Unsupported(expr, "Failed to execute subquery: " + executor.GetError().Message()));
        }
        
        auto query_result = executor.GetResult();
        if (query_result->type != QueryResultType::MATERIALIZED_RESULT) {
             return BindResult(BinderException::Unsupported(expr, "Internal error: expected materialized result from subquery execution"));
        }
        
        auto &mat_res = (MaterializedQueryResult&)*query_result;
        Value val;
        if (mat_res.RowCount() == 0) {
             val = Value(LogicalType::SQLNULL);
        } else {
             val = mat_res.GetValue(0, 0);
        }
        
        unique_ptr<Expression> bound_expr = make_uniq<BoundConstantExpression>(val);
        return BindResult(std::move(bound_expr));
    }
    default:
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
}



} // namespace duckdb
