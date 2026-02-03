#include "duckdb/planner/expression_binder/decide_constraints_binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

DecideConstraintsBinder::DecideConstraintsBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables)
    : DecideBinder(binder, context, variables), var_types(variables.size(), LogicalType::INTEGER){
}

static bool IsAllowedConstraintRHS(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

static bool IsAllowedOperatorChildren(const vector<unique_ptr<ParsedExpression>> &children,
                                      const case_insensitive_map_t<idx_t> &variables) {
    for (auto &child : children) {
        if (!IsAllowedConstraintRHS(*child, variables)) {
            return false;
        }
    }
    return true;
}

static bool IsAllowedConstraintRHS(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::CONSTANT:
            return true;
        case ExpressionClass::FUNCTION: {
            auto &func = expr.Cast<FunctionExpression>();
            if (func.is_operator) {
                if (StringUtil::Lower(func.function_name) == "-") {
                    return false;
                }
                if (!IsAllowedOperatorChildren(func.children, variables)) {
                    return false;
                }
                if (func.filter && !IsAllowedConstraintRHS(*func.filter, variables)) {
                    return false;
                }
                return true;
            }
            if (StringUtil::Lower(func.function_name) == "sum") {
                if (func.children.empty()) {
                    return false;
                }
                if (func.children.size() != 1) {
                    return false;
                }
                if (func.filter && !IsAllowedConstraintRHS(*func.filter, variables)) {
                    return false;
                }
                if (ExpressionContainsDecideVariable(*func.children[0], variables)) {
                    return false;
                }
                return true;
            }
            for (auto &child : func.children) {
                if (!IsAllowedConstraintRHS(*child, variables)) {
                    return false;
                }
            }
            if (func.filter && !IsAllowedConstraintRHS(*func.filter, variables)) {
                return false;
            }
            return true;
        }
        case ExpressionClass::OPERATOR: {
            auto &op = expr.Cast<OperatorExpression>();
            return IsAllowedOperatorChildren(op.children, variables);
        }
        case ExpressionClass::CAST: {
            auto &cast = expr.Cast<CastExpression>();
            return IsAllowedConstraintRHS(*cast.child, variables);
        }
        case ExpressionClass::SUBQUERY: {
            auto &subquery = expr.Cast<SubqueryExpression>();
            if (subquery.subquery_type != SubqueryType::SCALAR) {
                return false;
            }
            return true;
        }
        default:
            return false;
    }
}

BindResult DecideConstraintsBinder::BindComparison(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &comp = expr.Cast<ComparisonExpression>();
    // DebugPrintParsed("BindComparison.left", *comp.left);
    // DebugPrintParsed("BindComparison.right", *comp.right);
    string error_msg;
    auto left_type = GetExpressionType(*comp.left, error_msg);
    auto SimplifyZeroAddition = [&](auto &&self, unique_ptr<ParsedExpression> &node) -> void {
        if (!node) {
            return;
        }
        switch (node->GetExpressionClass()) {
        case ExpressionClass::FUNCTION: {
            auto &func = node->Cast<FunctionExpression>();
            for (auto &child : func.children) {
                self(self, child);
            }
            if (func.is_operator && func.function_name == "+" && func.children.size() == 2) {
                auto IsZeroConstant = [](const ParsedExpression &expr) {
                    if (expr.GetExpressionClass() != ExpressionClass::CONSTANT) {
                        return false;
                    }
                    auto &c = expr.Cast<const ConstantExpression>();
                    if (!c.value.type().IsNumeric()) {
                        return false;
                    }
                    return fabs(c.value.GetValue<double>()) < 1e-12;
                };
                auto &lhs = func.children[0];
                auto &rhs = func.children[1];
                if (IsZeroConstant(*lhs)) {
                    node = std::move(rhs);
                    self(self, node);
                    return;
                }
                if (IsZeroConstant(*rhs)) {
                    node = std::move(lhs);
                    self(self, node);
                    return;
                }
            }
            break;
        }
        case ExpressionClass::CAST: {
            auto &cast = node->Cast<CastExpression>();
            self(self, cast.child);
            break;
        }
        default:
            break;
        }
    };
    SimplifyZeroAddition(SimplifyZeroAddition, comp.right);
    // DebugPrintParsed("BindComparison.right (simplified)", *comp.right);
    auto &right = *comp.right;
    switch (comp.type) {
    case ExpressionType::COMPARE_EQUAL: {
        // Check if this is a type declaration (x IS INTEGER/REAL/BINARY)
        // The parser transforms "x IS INTEGER" into a comparison with a type marker constant
        if (comp.right->GetExpressionClass() == ExpressionClass::CONSTANT) {
            auto &const_expr = comp.right->Cast<ConstantExpression>();
            if (const_expr.value.type() == LogicalType::VARCHAR) {
                string type_marker = const_expr.value.ToString();

                // Check if it's a type marker from the grammar
                if (type_marker == "integer_variable" ||
                    type_marker == "real_variable" ||
                    type_marker == "binary_variable") {

                    // This is a type declaration, not a constraint
                    // Extract variable name from LHS
                    if (comp.left->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
                        auto &col_ref = comp.left->Cast<ColumnRefExpression>();
                        string var_name = col_ref.GetColumnName();

                        // Find variable index and update type
                        auto it = variables.find(var_name);
                        if (it != variables.end()) {
                            idx_t var_idx = it->second;

                            // Update var_types vector based on type marker
                            // DECIDE variables represent cardinality (count of tuples in package)
                            // Therefore, they MUST be integers - REAL types are not allowed
                            if (type_marker == "integer_variable") {
                                var_types[var_idx] = LogicalType::INTEGER;
                            } else if (type_marker == "real_variable") {
                                // REJECT: DECIDE variables must be integers (they represent cardinality)
                                throw BinderException(
                                    "DECIDE variable '%s' cannot be REAL type. "
                                    "DECIDE variables represent tuple cardinality (count) and must be INTEGER or BINARY.",
                                    var_name);
                            } else if (type_marker == "binary_variable") {
                                var_types[var_idx] = LogicalType::INTEGER;
                                // deb("Type declaration: variable '", var_name, "' is BINARY (treated as INTEGER with 0-1 bounds)");
                                
                                // Create implicit constraints: 0 <= x <= 1
                                // We replace the "x IS BINARY" expression with "(x >= 0) AND (x <= 1)"
                                auto x_ref = comp.left->Copy();
                                auto zero = make_uniq<ConstantExpression>(Value::INTEGER(0));
                                auto one = make_uniq<ConstantExpression>(Value::INTEGER(1));
                                
                                auto lower_bound = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, x_ref->Copy(), std::move(zero));
                                auto upper_bound = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(x_ref), std::move(one));
                                
                                auto bounds = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower_bound), std::move(upper_bound));
                                
                                // Bind the new constraints
                                unique_ptr<ParsedExpression> bounds_ptr = std::move(bounds);
                                return BindExpression(bounds_ptr, depth);
                            }

                            // Don't bind this as a constraint - it's metadata
                            // Return a dummy constant that will be filtered out
                            auto bound_expr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
                            return BindResult(std::move(bound_expr));
                        }
                    }
                }
            }
        }

        // Fallthrough to standard validation for actual equality constraints (e.g., SUM(x) = 10)
    }
    case ExpressionType::COMPARE_LESSTHAN:
    case ExpressionType::COMPARE_GREATERTHAN:
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO: {
        switch (left_type) {
            case DecideExpression::VARIABLE: {
                if (HasVariableExpression(right, variables)) {
                    return BindResult(BinderException::Unsupported(expr, StringUtil::Format("DECIDE variable cannot be compared to an expression with DECIDE variables, found '%s'", expr.ToString())));
                }
                break;
            }
            case DecideExpression::SUM: {
                if (comp.left->GetExpressionClass() != ExpressionClass::FUNCTION) {
                    return BindResult(BinderException::Unsupported(expr, "DECIDE constraint left-hand side must be SUM(...)"));
                }
                auto &lhs_func = comp.left->Cast<FunctionExpression>();
                if (!lhs_func.is_operator && StringUtil::Lower(lhs_func.function_name) != "sum") {
                    return BindResult(BinderException::Unsupported(expr, "DECIDE constraint left-hand side must be SUM(...)"));
                }
                if (!IsAllowedConstraintRHS(right, variables) || HasVariableExpression(right, variables)) {
                    return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUM cannot be compared to an expression that is not a scalar or aggregate without DECIDE variables, found '%s'", expr.ToString())));
                }
                break;
            }
            case DecideExpression::INVALID:
                return BindResult(BinderException::Unsupported(expr, error_msg));
            default:
                return BindResult(BinderException::Unsupported(expr, StringUtil::Format("Unsupported DecideExpression '%s'(%s)", comp.left->ToString(), EnumUtil::ToString(left_type))));
        }
        is_top_expression = false;
        return ExpressionBinder::BindExpression(expr_ptr, depth);
    }
    default:
        return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUCH THAT constraint clause does not support '%s'(ExpressionType::%s)", expr.ToString(), EnumUtil::ToString(comp.type))));
    }
}

BindResult DecideConstraintsBinder::BindOperator(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &op = expr.Cast<OperatorExpression>();
    switch (op.type) {
    case ExpressionType::COMPARE_IN:{
        if (!IsVariableExpression(*op.children.front(), variables)) {
            return BindResult(BinderException::Unsupported(expr, StringUtil::Format("Only DECIDE variables are allowed for IN expression, found '%s'", expr.ToString())));
        }
        for (size_t i = 1; i < op.children.size(); ++i) {
            auto &child = op.children[i];
            if (HasVariableExpression(*child, variables)) {
                return BindResult(BinderException::Unsupported(expr, StringUtil::Format("IN Right-hand side cannot contain DECIDE variables, found '%s'", expr.ToString())));
            }
        }
        is_top_expression = false;
        return ExpressionBinder::BindExpression(expr_ptr, depth);
    }
    default:
        return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUCH THAT constraint clause does not support '%s'(ExpressionType::%s)", expr.ToString(), EnumUtil::ToString(op.type))));
    }
}

BindResult DecideConstraintsBinder::BindBetween(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &between = expr.Cast<BetweenExpression>();

    // Transform BETWEEN into (input >= lower) AND (input <= upper)
    auto input_copy = between.input->Copy();
    
    auto lower_comp = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, std::move(between.input), std::move(between.lower));
    auto upper_comp = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(input_copy), std::move(between.upper));

    auto conjunction = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower_comp), std::move(upper_comp));
    
    // Bind the new conjunction
    // We need to replace the current expression pointer with the new conjunction
    expr_ptr = std::move(conjunction);
    return BindConjunction(expr_ptr, depth);
}

BindResult DecideConstraintsBinder::BindConjunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &conj = expr.Cast<ConjunctionExpression>();
    // first try to bind the children of the case expression
    ErrorData error;
    for (idx_t i = 0; i < conj.children.size(); i++) {
        is_top_expression = true;
        BindChild(conj.children[i], depth, error);
    }
    if (error.HasError()) {
        return BindResult(std::move(error));
    }
    // the children have been successfully resolved
    // cast the input types to boolean (if necessary)
    // and construct the bound conjunction expression
    auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
    for (auto &child_expr : conj.children) {
        auto &child = BoundExpression::GetExpression(*child_expr);
        result->children.push_back(BoundCastExpression::AddCastToType(context, std::move(child), LogicalType::BOOLEAN));
    }
    // now create the bound conjunction expression
    return BindResult(std::move(result));
}

BindResult DecideConstraintsBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
    case ExpressionClass::COLUMN_REF:
    case ExpressionClass::CONSTANT: {
        if (!is_top_expression) {
            return ExpressionBinder::BindExpression(expr_ptr, depth);
        }
        break;
    }
    case ExpressionClass::FUNCTION: {
        if (!is_top_expression) {
            return BindFunction(expr_ptr, depth);
        }
        break;
	}
    case ExpressionClass::COMPARISON:
        return BindComparison(expr_ptr, depth);
    case ExpressionClass::OPERATOR: {
        return BindOperator(expr_ptr, depth);
    }
	case ExpressionClass::BETWEEN:
        return BindBetween(expr_ptr, depth);
    case ExpressionClass::CONJUNCTION:
        return BindConjunction(expr_ptr, depth);
    case ExpressionClass::SUBQUERY: {
        return DecideBinder::BindExpression(expr_ptr, depth, root_expression);
    }
	default:
        break;
	}
    return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUCH THAT clause does not support '%s'(ExpressionClass::%s)", expr.ToString(), EnumUtil::ToString(expr.GetExpressionClass()))));
}

DecideExpression DecideConstraintsBinder::GetExpressionType(ParsedExpression &expr, string& error_msg){
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::COLUMN_REF: {
        if (!IsVariableExpression(expr, variables)) {
            error_msg = StringUtil::Format("SUCH THAT clause: Column '%s' must be one of the DECIDE variables", expr.ToString());
            return DecideExpression::INVALID;
        }
        return DecideExpression::VARIABLE;
    }
    case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		if (StringUtil::Lower(func.function_name) == "sum") {
            if (!ValidateSumArgument(*func.children.front(), variables, error_msg)) {
                error_msg += ", found '" + expr.ToString() + "'";
                return DecideExpression::INVALID;
            }
            return DecideExpression::SUM;
		} else {
            error_msg = StringUtil::Format("SUCH THAT clause does not support left-hand side function '%s', only SUM is allowed.", func.function_name);
            return DecideExpression::INVALID;
        }
    }
    default: {
        error_msg = StringUtil::Format("The left-hand side of a SUCH THAT constraint must be a DECIDE variable or a SUM expression over a DECIDE variable (e.g., SUM(x * a) / SUM(x)). Found '%s' instead.", expr.ToString());
    	return DecideExpression::INVALID;
    }
    }
}

} // namespace duckdb
