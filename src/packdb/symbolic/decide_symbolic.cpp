//===----------------------------------------------------------------------===//
//                         PackDB
//
// packdb/symbolic/decide_symbolic.cpp
//
// Symbolic Translation Layer for DECIDE Expressions
//
//===----------------------------------------------------------------------===//

#include "duckdb/packdb/symbolic/decide_symbolic.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "symbolicc++.h"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/packdb/utility/debug.hpp"
#include <sstream>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <vector>
#include <limits>
#include <cstdlib>
#include <map>

namespace duckdb {

// Forward declare the recursive function
Symbolic ToSymbolicRecursive(const ParsedExpression &expr, SymbolicTranslationContext &ctx);
unique_ptr<ParsedExpression> FromSymbolic(const Symbolic &s, SymbolicTranslationContext &ctx);

static bool TryGetNonNegativeInteger(const Symbolic &value, int64_t &out_integer) {
    if (value.type() != typeid(Numeric)) {
        return false;
    }
    double numeric_value = double(value);
    double rounded = std::llround(numeric_value);
    if (fabs(numeric_value - rounded) > 1e-9) {
        return false;
    }
    if (rounded < 0) {
        return false;
    }
    out_integer = (int64_t)rounded;
    return true;
}

static Symbolic ApplySymbolicPower(const Symbolic &base, const Symbolic &exponent) {
    int64_t integer_exponent;
    if (TryGetNonNegativeInteger(exponent, integer_exponent)) {
        if (integer_exponent == 0) {
            return Symbolic(1.0);
        }
        Symbolic result = base;
        for (int64_t i = 1; i < integer_exponent; i++) {
            result = result * base;
        }
        return result;
    }
    return pow(base, exponent);
}

static bool SymbolicContainsDecideVariable(const Symbolic &s, const case_insensitive_map_t<idx_t> &decide_variables) {
    if (s.type() == typeid(Symbol)) {
        auto name = CastPtr<const Symbol>(s)->name;
        if (decide_variables.count(name) > 0) {
            return true;
        }
        // ABS placeholders contain decide variables by construction
        // (only created when the inner expression references a decide variable)
        if (name.rfind("__ABS_", 0) == 0) {
            return true;
        }
        return false;
    }
    if (s.type() == typeid(Numeric)) {
        return false;
    }
    if (s.type() == typeid(Product)) {
        CastPtr<const Product> prod(s);
        for (auto &factor : prod->factors) {
            if (SymbolicContainsDecideVariable(factor, decide_variables)) {
                return true;
            }
        }
        return false;
    }
    if (s.type() == typeid(Sum)) {
        CastPtr<const Sum> sum(s);
        for (auto &term : sum->summands) {
            if (SymbolicContainsDecideVariable(term, decide_variables)) {
                return true;
            }
        }
        return false;
    }
    if (s.type() == typeid(Power)) {
        CastPtr<const Power> power_node(s);
        return SymbolicContainsDecideVariable(power_node->parameters.front(), decide_variables) ||
               SymbolicContainsDecideVariable(power_node->parameters.back(), decide_variables);
    }
    // Fallback: inspect any Symbol-derived parameters
    try {
        CastPtr<const Symbol> sym(s);
        for (auto &param : sym->parameters) {
            if (SymbolicContainsDecideVariable(param, decide_variables)) {
                return true;
            }
        }
    } catch (...) {
        // Ignore if cast fails
    }
    return false;
}

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//

bool IsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
    if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
        return false;
    }

    auto &colref = expr.Cast<ColumnRefExpression>();
    if (colref.IsQualified()) {
        // Check qualified form: Table.var (for table-scoped variables)
        string qualified = colref.GetTableName() + "." + colref.GetColumnName();
        return variables.count(qualified) > 0;
    }

    const auto &name = colref.GetColumnName();
    return variables.count(name) > 0;
}

bool IsRowVarying(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &decide_variables) {
    // deb("IsRowVarying: Checking expression:", expr.ToString());
    
    bool has_row_varying = false;
    
    // Use ParsedExpressionIterator to traverse the expression tree
    ParsedExpressionIterator::EnumerateChildren(expr, 
        [&](const ParsedExpression &child) {
            if (child.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
                if (!IsDecideVariable(child, decide_variables)) {
                    // deb("  -> Found row-varying column:", child.ToString());
                    has_row_varying = true;
                }
            }
        });
    
    // deb("  -> Result:", has_row_varying);
    return has_row_varying;
}

//===--------------------------------------------------------------------===//
// ToSymbolic Implementation
//===--------------------------------------------------------------------===//

Symbolic ToSymbolicRecursive(const ParsedExpression &expr, SymbolicTranslationContext &ctx) {
    // deb("\n=== ToSymbolic ===");
    // deb("Input expression:", expr.ToString());
    // deb("Expression class:", EnumUtil::ToString(expr.GetExpressionClass()));
    
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::CONSTANT: {
            auto &const_expr = expr.Cast<ConstantExpression>();
            // deb("Processing CONSTANT:", const_expr.value.ToString());
            
            // Extract numeric value
            double value;
            switch (const_expr.value.type().id()) {
                case LogicalTypeId::INTEGER:
                    value = const_expr.value.GetValue<int32_t>();
                    break;
                case LogicalTypeId::BIGINT:
                    value = const_expr.value.GetValue<int64_t>();
                    break;
                case LogicalTypeId::DOUBLE:
                    value = const_expr.value.GetValue<double>();
                    break;
                case LogicalTypeId::FLOAT:
                    value = const_expr.value.GetValue<float>();
                    break;
                case LogicalTypeId::VARCHAR: {
                    auto s = const_expr.value.GetValue<string>();
                    // deb("  -> Converted string constant to symbol:", s);
                    return Symbolic(s.c_str());
                }
                case LogicalTypeId::SMALLINT:
                    value = const_expr.value.GetValue<int16_t>();
                    break;
                case LogicalTypeId::TINYINT:
                    value = const_expr.value.GetValue<int8_t>();
                    break;
                case LogicalTypeId::DECIMAL:
                    value = const_expr.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    break;
                case LogicalTypeId::HUGEINT: {
                    auto v = const_expr.value.DefaultCastAs(LogicalType::DOUBLE);
                    value = v.GetValue<double>();
                    break;
                }
                default:
                    throw InternalException("ToSymbolic: Unsupported constant type: %s", 
                        const_expr.value.type().ToString());
            }
            
            // deb("  -> Converted to numeric:", value);
            return Symbolic(value);
        }
        
        case ExpressionClass::COLUMN_REF: {
            auto &colref = expr.Cast<ColumnRefExpression>();
            const auto &name = colref.GetColumnName();
            // deb("Processing COLUMN_REF:", name);
            
            if (IsDecideVariable(expr, ctx.decide_variables)) {
                // deb("  -> This is a DECIDE variable, creating symbol:", name);
                return Symbolic(name);
            } else {
                // deb("  -> This is a row-varying column, creating symbol:", name);
                return Symbolic(name);
            }
        }
        
        case ExpressionClass::OPERATOR: {
            auto &op_expr = expr.Cast<OperatorExpression>();
            // deb("Processing OPERATOR:", ExpressionTypeToString(op_expr.type));
            // deb("Number of children:", op_expr.children.size());

            if (op_expr.type == ExpressionType::OPERATOR_NOT) {
                D_ASSERT(op_expr.children.size() == 1);
                auto child = ToSymbolicRecursive(*op_expr.children[0], ctx);
                std::stringstream cs;
                cs << child;
                return Symbolic("NOT(" + cs.str() + ")");
            }
            if (op_expr.type == ExpressionType::COMPARE_IN || op_expr.type == ExpressionType::COMPARE_NOT_IN) {
                // Represent IN as a special symbolic predicate: IN(left, rhs1, rhs2, ...)
                if (op_expr.children.size() < 2) {
                    throw InternalException("ToSymbolic: IN requires at least 2 children");
                }
                auto left = ToSymbolicRecursive(*op_expr.children[0], ctx);
                std::stringstream ls;
                ls << left;
                std::stringstream args_ss;
                for (idx_t i = 1; i < op_expr.children.size(); i++) {
                    if (i > 1) args_ss << ",";
                    auto right = ToSymbolicRecursive(*op_expr.children[i], ctx);
                    std::stringstream rs;
                    rs << right;
                    args_ss << rs.str();
                }
                string tag = (op_expr.type == ExpressionType::COMPARE_NOT_IN) ? "NOT_IN" : "IN";
                return Symbolic(tag + "(" + ls.str() + "," + args_ss.str() + ")");
            }
            throw InternalException("ToSymbolic: Unsupported operator type: %s", ExpressionTypeToString(op_expr.type));
        }

        case ExpressionClass::CAST: {
            auto &cast_expr = expr.Cast<CastExpression>();
            // deb("Processing CAST to:", cast_expr.cast_type.ToString());
            return ToSymbolicRecursive(*cast_expr.child, ctx);
        }

        case ExpressionClass::COMPARISON: {
            auto &cmp = expr.Cast<ComparisonExpression>();
            // deb("Processing COMPARISON:", ExpressionTypeToString(cmp.type));
            auto left = ToSymbolicRecursive(*cmp.left, ctx);
            auto right = ToSymbolicRecursive(*cmp.right, ctx);
            std::stringstream ls, rs;
            ls << left;
            rs << right;
            string tag;
            switch (cmp.type) {
                case ExpressionType::COMPARE_EQUAL:               tag = "EQ"; break;
                case ExpressionType::COMPARE_NOTEQUAL:            tag = "NE"; break;
                case ExpressionType::COMPARE_LESSTHAN:            tag = "LT"; break;
                case ExpressionType::COMPARE_LESSTHANOREQUALTO:   tag = "LE"; break;
                case ExpressionType::COMPARE_GREATERTHAN:         tag = "GT"; break;
                case ExpressionType::COMPARE_GREATERTHANOREQUALTO:tag = "GE"; break;
                case ExpressionType::COMPARE_BETWEEN:
                case ExpressionType::COMPARE_NOT_BETWEEN:
                case ExpressionType::COMPARE_IN:
                case ExpressionType::COMPARE_NOT_IN:
                case ExpressionType::COMPARE_DISTINCT_FROM:
                case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
                    throw InternalException("ToSymbolic: DISTINCT/IN/NOT IN handled elsewhere; BETWEEN has its own class");
                default:
                    throw InternalException("ToSymbolic: Unsupported comparison type: %s", ExpressionTypeToString(cmp.type));
            }
            return Symbolic(tag + "(" + ls.str() + "," + rs.str() + ")");
        }

        case ExpressionClass::BETWEEN: {
            auto &between = expr.Cast<BetweenExpression>();
            // deb("Processing BETWEEN expression");
            auto input = ToSymbolicRecursive(*between.input, ctx);
            auto lower = ToSymbolicRecursive(*between.lower, ctx);
            auto upper = ToSymbolicRecursive(*between.upper, ctx);
            std::stringstream is, ls, us;
            is << input; ls << lower; us << upper;
            return Symbolic("BETWEEN(" + is.str() + "," + ls.str() + "," + us.str() + ")");
        }

        case ExpressionClass::CONJUNCTION: {
            auto &conj = expr.Cast<ConjunctionExpression>();
            // deb("Processing CONJUNCTION:", ExpressionTypeToString(conj.type));
            D_ASSERT(!conj.children.empty());
            string tag;
            if (conj.type == ExpressionType::CONJUNCTION_AND) tag = "AND";
            else if (conj.type == ExpressionType::CONJUNCTION_OR) tag = "OR";
            else throw InternalException("ToSymbolic: Unsupported conjunction type");
            std::stringstream ss;
            for (idx_t i = 0; i < conj.children.size(); i++) {
                if (i > 0) ss << ",";
                auto next = ToSymbolicRecursive(*conj.children[i], ctx);
                std::stringstream ns;
                ns << next;
                ss << ns.str();
            }
            return Symbolic(tag + "(" + ss.str() + ")");
        }
        
        case ExpressionClass::FUNCTION: {
            auto &func_expr = expr.Cast<FunctionExpression>();
            // deb("Processing FUNCTION:", func_expr.function_name);
            // deb("Number of arguments:", func_expr.children.size());
        
            string func_name_lower = StringUtil::Lower(func_expr.function_name);
        
            // Handle built-in arithmetic operators rendered as functions
            if (func_expr.is_operator) {
                vector<Symbolic> args;
                args.reserve(func_expr.children.size());
                for (auto &child : func_expr.children) {
                    args.push_back(ToSymbolicRecursive(*child, ctx));
                }
        
                if (func_expr.function_name == "+") {
                    D_ASSERT(args.size() == 2);
                    return args[0] + args[1];
                } else if (func_expr.function_name == "-") {
                    if (args.size() == 1) {
                        return -args[0];
                    }
                    D_ASSERT(args.size() == 2);
                    return args[0] - args[1];
                } else if (func_expr.function_name == "*") {
                    D_ASSERT(args.size() == 2);
                    return args[0] * args[1];
                } else if (func_expr.function_name == "/") {
                    D_ASSERT(args.size() == 2);
                    return args[0] / args[1];
                } else if (func_expr.function_name == "^" || func_expr.function_name == "**") {
                    D_ASSERT(args.size() == 2);
                    return ApplySymbolicPower(args[0], args[1]);
                } else {
                    throw InternalException("ToSymbolic: Unsupported operator function: %s",
                        func_expr.function_name);
                }
            }

            if (func_name_lower == "sum") {
                if (func_expr.children.empty()) {
                    throw InternalException("ToSymbolic: SUM function with no arguments");
                }
                auto inner_symbolic = ToSymbolicRecursive(*func_expr.children[0], ctx);
                return Symbolic("__SUM__") * inner_symbolic;
            } else if (func_name_lower == "min") {
                if (func_expr.children.empty()) {
                    throw InternalException("ToSymbolic: MIN function with no arguments");
                }
                auto inner_symbolic = ToSymbolicRecursive(*func_expr.children[0], ctx);
                return Symbolic("__MIN__") * inner_symbolic;
            } else if (func_name_lower == "max") {
                if (func_expr.children.empty()) {
                    throw InternalException("ToSymbolic: MAX function with no arguments");
                }
                auto inner_symbolic = ToSymbolicRecursive(*func_expr.children[0], ctx);
                return Symbolic("__MAX__") * inner_symbolic;
            } else if (func_name_lower == "avg") {
                if (func_expr.children.empty()) {
                    throw InternalException("ToSymbolic: AVG function with no arguments");
                }
                auto inner_symbolic = ToSymbolicRecursive(*func_expr.children[0], ctx);
                return Symbolic("__AVG__") * inner_symbolic;
            } else if (func_name_lower == "pow" || func_name_lower == "power") {
                if (func_expr.children.size() != 2) {
                    throw InternalException("ToSymbolic: POW/POWER expects two arguments");
                }
                auto base = ToSymbolicRecursive(*func_expr.children[0], ctx);
                auto exponent = ToSymbolicRecursive(*func_expr.children[1], ctx);
                return ApplySymbolicPower(base, exponent);
            } else if (func_name_lower == "abs") {
                // ABS is nonlinear — treat as opaque placeholder (like subqueries).
                // Normalization sees it as a plain variable; the optimizer linearizes it later.
                if (func_expr.children.size() != 1) {
                    throw InternalException("ToSymbolic: ABS function requires one argument");
                }
                string placeholder = "__ABS_" + to_string(ctx.abs_map.size()) + "__";
                ctx.abs_map[placeholder] = expr.Copy();
                return Symbolic(placeholder);
            } else {
                throw InternalException("ToSymbolic: Unsupported function: %s", func_expr.function_name);
            }
        }
        
        case ExpressionClass::SUBQUERY: {
            string placeholder = "__SUBQUERY_" + to_string(ctx.subquery_map.size()) + "__";
            ctx.subquery_map[placeholder] = expr.Copy();
            return Symbolic(placeholder);
        }
        
        default:
            throw InternalException("ToSymbolic: Unsupported expression class: %s", 
                EnumUtil::ToString(expr.GetExpressionClass()));
    }
}

Symbolic ToSymbolicObj(const ParsedExpression &expr, SymbolicTranslationContext &ctx) {
    return ToSymbolicRecursive(expr, ctx);
}

//===--------------------------------------------------------------------===//
// FromSymbolic Implementation
//===--------------------------------------------------------------------===//

static unique_ptr<ParsedExpression> FromSymbolicNumber(double v) {
    return make_uniq_base<ParsedExpression, ConstantExpression>(Value::DOUBLE(v));
}

static unique_ptr<ParsedExpression> MakeOp(const string &op, unique_ptr<ParsedExpression> lhs, unique_ptr<ParsedExpression> rhs) {
    // Build arithmetic using operator FunctionExpression with is_operator=true
    vector<unique_ptr<ParsedExpression>> args;
    args.push_back(std::move(lhs));
    if (rhs) args.push_back(std::move(rhs));
    auto fe = make_uniq<FunctionExpression>(op, std::move(args), nullptr, nullptr, false, true);
    return std::move(fe);
}

static unique_ptr<ParsedExpression> BuildProductExpression(const vector<const Symbolic *> &factors,
                                                           SymbolicTranslationContext &ctx) {
    unique_ptr<ParsedExpression> acc;
    for (auto *factor : factors) {
        auto child = FromSymbolic(*factor, ctx);
        if (!acc) {
            acc = std::move(child);
        } else {
            acc = MakeOp("*", std::move(acc), std::move(child));
        }
    }
    return acc;
}

static void CollectAdditiveTerms(const Symbolic &expr, vector<Symbolic> &terms) {
    if (expr.type() == typeid(Sum)) {
        CastPtr<const Sum> sum(expr);
        for (auto &term : sum->summands) {
            CollectAdditiveTerms(term, terms);
        }
        return;
    }
    terms.push_back(expr);
}

static Symbolic SumSymbolicTerms(const vector<Symbolic> &terms) {
    if (terms.empty()) {
        return Symbolic(0.0);
    }
    Symbolic result = terms[0];
    for (idx_t i = 1; i < terms.size(); i++) {
        result = result + terms[i];
    }
    return result;
}

static Symbolic MultiplySymbolicFactors(const vector<Symbolic> &factors) {
    if (factors.empty()) {
        return Symbolic(1.0);
    }
    Symbolic result = factors[0];
    for (idx_t i = 1; i < factors.size(); i++) {
        result = result * factors[i];
    }
    return result;
}

static bool SymbolicIsApproxNumeric(const Symbolic &s, double target) {
    if (s.type() != typeid(Numeric)) {
        return false;
    }
    return fabs(double(s) - target) < 1e-12;
}

static bool SymbolicIsZero(const Symbolic &s) {
    return SymbolicIsApproxNumeric(s, 0.0);
}

static bool SymbolicIsOne(const Symbolic &s) {
    return SymbolicIsApproxNumeric(s, 1.0);
}

static bool SymbolicContainsDecideVariable(const Symbolic &s, const case_insensitive_map_t<idx_t> &decide_variables);
static bool IsSumMarker(const Symbolic &s);

static void CollectDecideFactors(const Symbolic &node, vector<Symbolic> &decide_factors,
                                 vector<Symbolic> &other_factors, const case_insensitive_map_t<idx_t> &decide_variables) {
    if (node.type() == typeid(Product)) {
        CastPtr<const Product> prod(node);
        for (auto &factor : prod->factors) {
            CollectDecideFactors(factor, decide_factors, other_factors, decide_variables);
        }
        return;
    }
    if (node.type() == typeid(Symbol)) {
        auto name = CastPtr<const Symbol>(node)->name;
        if (decide_variables.count(name) > 0 || name.rfind("__ABS_", 0) == 0) {
            decide_factors.push_back(node);
        } else {
            other_factors.push_back(node);
        }
        return;
    }
    if (node.type() == typeid(Numeric)) {
        other_factors.push_back(node);
        return;
    }
    if (SymbolicContainsDecideVariable(node, decide_variables)) {
        decide_factors.push_back(node);
    } else {
        other_factors.push_back(node);
    }
}

static pair<Symbolic, Symbolic> ExtractDecideFactors(const Symbolic &term, const case_insensitive_map_t<idx_t> &decide_variables) {
    vector<Symbolic> decide_factors;
    vector<Symbolic> other_factors;
    CollectDecideFactors(term, decide_factors, other_factors, decide_variables);
    Symbolic decide_part = MultiplySymbolicFactors(decide_factors);
    Symbolic other_part = MultiplySymbolicFactors(other_factors);
    return {decide_part, other_part};
}

struct FactoredTerm {
    Symbolic decide_part;
    Symbolic coefficient;
};

static bool ExtractSumInner(const Symbolic &term, Symbolic &inner_out) {
    if (term.type() == typeid(Product)) {
        CastPtr<const Product> prod(term);
        vector<Symbolic> remaining;
        bool has_sum = false;
        for (auto &factor : prod->factors) {
            if (IsSumMarker(factor)) {
                has_sum = true;
            } else {
                remaining.push_back(factor);
            }
        }
        if (has_sum) {
            inner_out = MultiplySymbolicFactors(remaining);
            return true;
        }
    }
    if (IsSumMarker(term)) {
        inner_out = Symbolic(1.0);
        return true;
    }
    return false;
}

static vector<FactoredTerm> CollectFactoredTerms(const Symbolic &expr, const case_insensitive_map_t<idx_t> &decide_variables) {
    vector<FactoredTerm> result;
    if (expr.type() != typeid(Sum)) {
        result.push_back(FactoredTerm{expr, Symbolic(1.0)});
        return result;
    }
    CastPtr<const Sum> sum(expr);
    std::map<string, FactoredTerm> grouped_terms;
    vector<string> order;
    for (auto &term : sum->summands) {
        auto parts = ExtractDecideFactors(term, decide_variables);
        Symbolic decide_part = parts.first.simplify();
        Symbolic coefficient = parts.second.simplify();
        std::stringstream ss;
        ss << decide_part;
        auto key = ss.str();
        auto it = grouped_terms.find(key);
        if (it == grouped_terms.end()) {
            grouped_terms.emplace(key, FactoredTerm{decide_part, coefficient});
            order.push_back(key);
        } else {
            it->second.coefficient = (it->second.coefficient + coefficient).simplify();
        }
    }
    for (auto &key : order) {
        auto &entry = grouped_terms[key];
        auto coeff = entry.coefficient.simplify();
        if (SymbolicIsZero(coeff)) {
            continue;
        }
        result.push_back(FactoredTerm{entry.decide_part.simplify(), coeff});
    }
    return result;
}

static unique_ptr<ParsedExpression> BuildFactoredTermExpression(const FactoredTerm &term, SymbolicTranslationContext &ctx) {
    bool decide_is_one = SymbolicIsOne(term.decide_part);
    bool coeff_is_one = SymbolicIsOne(term.coefficient);

    if (decide_is_one && coeff_is_one) {
        return FromSymbolicNumber(1.0);
    }
    if (decide_is_one) {
        return FromSymbolic(term.coefficient, ctx);
    }
    if (coeff_is_one) {
        return FromSymbolic(term.decide_part, ctx);
    }
    auto decide_expr = FromSymbolic(term.decide_part, ctx);
    auto coeff_expr = FromSymbolic(term.coefficient, ctx);
    return MakeOp("*", std::move(decide_expr), std::move(coeff_expr));
}

static unique_ptr<ParsedExpression> BuildFactoredSumExpression(const Symbolic &expr, SymbolicTranslationContext &ctx) {
    if (expr.type() != typeid(Sum)) {
        return FromSymbolic(expr, ctx);
    }
    auto terms = CollectFactoredTerms(expr, ctx.decide_variables);
    unique_ptr<ParsedExpression> aggregated;
    for (auto &term : terms) {
        auto term_expr = BuildFactoredTermExpression(term, ctx);
        if (!term_expr) {
            continue;
        }
        if (!aggregated) {
            aggregated = std::move(term_expr);
        } else {
            aggregated = MakeOp("+", std::move(aggregated), std::move(term_expr));
        }
    }
    if (!aggregated) {
        return FromSymbolicNumber(0.0);
    }
    return aggregated;
}

static unique_ptr<ParsedExpression> FromSymbolicProduct(const Product &prod, SymbolicTranslationContext &ctx) {
    vector<const Symbolic *> decide_factors;
    vector<const Symbolic *> other_factors;
    vector<const Symbolic *> all_factors;
    for (auto &factor : prod.factors) {
        all_factors.push_back(&factor);
        if (SymbolicContainsDecideVariable(factor, ctx.decide_variables)) {
            decide_factors.push_back(&factor);
        } else {
            other_factors.push_back(&factor);
        }
    }

    if (decide_factors.empty() || other_factors.empty()) {
        auto acc = BuildProductExpression(all_factors, ctx);
        if (!acc) {
            return FromSymbolicNumber(1.0);
        }
        return acc;
    }

    auto left_expr = BuildProductExpression(decide_factors, ctx);
    auto right_expr = BuildProductExpression(other_factors, ctx);
    if (!left_expr) {
        left_expr = FromSymbolicNumber(1.0);
    }
    if (!right_expr) {
        right_expr = FromSymbolicNumber(1.0);
    }
    return MakeOp("*", std::move(left_expr), std::move(right_expr));
}

static unique_ptr<ParsedExpression> FromSymbolicSum(const Sum &sum, SymbolicTranslationContext &ctx) {
    // Left fold using "+"
    unique_ptr<ParsedExpression> acc;
    for (auto &term : sum.summands) {
        auto child = FromSymbolic(term, ctx);
        if (!acc) acc = std::move(child);
        else acc = MakeOp("+", std::move(acc), std::move(child));
    }
    if (!acc) return FromSymbolicNumber(0.0);
    return acc;
}

static bool IsSumMarker(const Symbolic &s) {
    return s.type() == typeid(Symbol) && CastPtr<const Symbol>(s)->name == "__SUM__";
}

static bool IsMinMarker(const Symbolic &s) {
    return s.type() == typeid(Symbol) && CastPtr<const Symbol>(s)->name == "__MIN__";
}

static bool IsMaxMarker(const Symbolic &s) {
    return s.type() == typeid(Symbol) && CastPtr<const Symbol>(s)->name == "__MAX__";
}

static bool IsAvgMarker(const Symbolic &s) {
    return s.type() == typeid(Symbol) && CastPtr<const Symbol>(s)->name == "__AVG__";
}

static bool IsAggregateMarker(const Symbolic &s) {
    return IsSumMarker(s) || IsMinMarker(s) || IsMaxMarker(s) || IsAvgMarker(s);
}

static unique_ptr<ParsedExpression> FromSymbolicAggregateProduct(const Product &prod, SymbolicTranslationContext &ctx) {
    // Expect one aggregate marker (__SUM__, __MIN__, __MAX__, __AVG__) and remaining factors as inner expression
    list<Symbolic> non_markers;
    bool has_sum_marker = false;
    bool has_min_marker = false;
    bool has_max_marker = false;
    bool has_avg_marker = false;
    for (auto &f : prod.factors) {
        if (IsSumMarker(f)) has_sum_marker = true;
        else if (IsMinMarker(f)) has_min_marker = true;
        else if (IsMaxMarker(f)) has_max_marker = true;
        else if (IsAvgMarker(f)) has_avg_marker = true;
        else non_markers.push_back(f);
    }
    if (!(has_sum_marker || has_min_marker || has_max_marker || has_avg_marker) || non_markers.empty()) {
        // Fallback to regular product
        return FromSymbolicProduct(prod, ctx);
    }
    // Rebuild aggregate(inner)
    auto it = non_markers.begin();
    Symbolic inner = *it++;
    for (; it != non_markers.end(); ++it) inner = inner * (*it);
    vector<unique_ptr<ParsedExpression>> args;
    args.push_back(FromSymbolic(inner, ctx));
    string func_name = has_sum_marker ? "sum" : (has_min_marker ? "min" : (has_max_marker ? "max" : "avg"));
    return make_uniq_base<ParsedExpression, FunctionExpression>(func_name, std::move(args));
}

unique_ptr<ParsedExpression> FromSymbolic(const Symbolic &s, SymbolicTranslationContext &ctx) {
    if (s.type() == typeid(Numeric)) {
        return FromSymbolicNumber(double(s));
    }
    if (s.type() == typeid(Symbol)) {
        auto name = CastPtr<const Symbol>(s)->name;
        // Check if it's a subquery placeholder
        if (ctx.subquery_map.count(name)) {
            return ctx.subquery_map[name]->Copy();
        }
        // Check if it's an ABS placeholder
        if (ctx.abs_map.count(name)) {
            return ctx.abs_map[name]->Copy();
        }
        // Treat any plain symbol as a column/variable reference
        return make_uniq_base<ParsedExpression, ColumnRefExpression>(name);
    }
    if (s.type() == typeid(Product)) {
        // Special-case aggregate marker
        CastPtr<const Product> prod(s);
        bool has_agg_marker = false;
        for (auto &f : prod->factors) if (IsAggregateMarker(f)) { has_agg_marker = true; break; }
        if (has_agg_marker) return FromSymbolicAggregateProduct(*prod, ctx);
        return FromSymbolicProduct(*prod, ctx);
    }
    if (s.type() == typeid(Sum)) {
        CastPtr<const Sum> sum(s);
        return FromSymbolicSum(*sum, ctx);
    }
    if (s.type() == typeid(Power)) {
        CastPtr<const Power> power_node(s);
        const auto &base = power_node->parameters.front();
        const auto &exponent = power_node->parameters.back();
        int64_t exponent_int;
        if (!TryGetNonNegativeInteger(exponent, exponent_int)) {
            throw InternalException("FromSymbolic: Non-integer exponents are not supported in DECIDE normalization");
        }
        if (exponent_int == 0) {
            return FromSymbolicNumber(1.0);
        }
        auto result = FromSymbolic(base, ctx);
        for (int64_t i = 1; i < exponent_int; i++) {
            result = MakeOp("*", std::move(result), FromSymbolic(base, ctx));
        }
        return result;
    }
    // Fallback: stringify (debug) is not acceptable; throw
    throw InternalException("FromSymbolic: Unsupported symbolic node");
}

//===--------------------------------------------------------------------===//
// Normalization over ParsedExpression using Symbolic
//===--------------------------------------------------------------------===//

static bool IsNumericConstant(const ParsedExpression &expr, double &out) {
    if (expr.GetExpressionClass() != ExpressionClass::CONSTANT) return false;
    auto &c = expr.Cast<ConstantExpression>();
    if (c.value.IsNull()) return false;
    switch (c.value.type().id()) {
        case LogicalTypeId::TINYINT: out = c.value.GetValue<int8_t>(); return true;
        case LogicalTypeId::SMALLINT: out = c.value.GetValue<int16_t>(); return true;
        case LogicalTypeId::INTEGER: out = c.value.GetValue<int32_t>(); return true;
        case LogicalTypeId::BIGINT: out = c.value.GetValue<int64_t>(); return true;
        case LogicalTypeId::FLOAT: out = c.value.GetValue<float>(); return true;
        case LogicalTypeId::DOUBLE: out = c.value.GetValue<double>(); return true;
        case LogicalTypeId::DECIMAL: out = c.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>(); return true;
        case LogicalTypeId::HUGEINT: out = c.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>(); return true;
        default: return false;
    }
}

static unique_ptr<ParsedExpression> MakeDoubleConstant(double v) {
    return make_uniq_base<ParsedExpression, ConstantExpression>(Value::DOUBLE(v));
}

// Check if an expression tree contains a SUM() function anywhere
static bool ContainsSumFunction(const ParsedExpression &expr) {
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::FUNCTION: {
            auto &func = expr.Cast<const FunctionExpression>();
            if (!func.is_operator && StringUtil::Lower(func.function_name) == "sum") {
                return true;
            }
            for (auto &child : func.children) {
                if (ContainsSumFunction(*child)) return true;
            }
            if (func.filter && ContainsSumFunction(*func.filter)) return true;
            return false;
        }
        case ExpressionClass::OPERATOR: {
            auto &op = expr.Cast<const OperatorExpression>();
            for (auto &child : op.children) {
                if (ContainsSumFunction(*child)) return true;
            }
            return false;
        }
        case ExpressionClass::CAST: {
            auto &cast = expr.Cast<const CastExpression>();
            return ContainsSumFunction(*cast.child);
        }
        case ExpressionClass::CONJUNCTION: {
            auto &conj = expr.Cast<const ConjunctionExpression>();
            for (auto &child : conj.children) {
                if (ContainsSumFunction(*child)) return true;
            }
            return false;
        }
        default:
            return false;
    }
}

// Normalize comparator between LHS and numeric RHS by isolating SUM terms containing DECIDE variables
static unique_ptr<ParsedExpression> NormalizeComparisonExpr(const ComparisonExpression &cmp,
                                                            const case_insensitive_map_t<idx_t> &decide_variables) {
    // Only handle <=, <, >=, > with numeric RHS
    if (cmp.type != ExpressionType::COMPARE_LESSTHAN && cmp.type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
        cmp.type != ExpressionType::COMPARE_GREATERTHAN && cmp.type != ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
        return cmp.Copy();
    }
    double rhs_num;
    if (!IsNumericConstant(*cmp.right, rhs_num)) {
        return cmp.Copy();
    }

    // Only normalize if the LHS contains a SUM() somewhere in the tree.
    // For bare per-row constraints (e.g., "x < 6"), leave unchanged.
    if (!ContainsSumFunction(*cmp.left)) {
        return cmp.Copy();
    }

    SymbolicTranslationContext ctx(decide_variables);
    Symbolic lhs_sym = ToSymbolicObj(*cmp.left, ctx).expand().simplify();

    vector<Symbolic> additive_terms;
    CollectAdditiveTerms(lhs_sym, additive_terms);

    vector<Symbolic> decide_inners;
    vector<Symbolic> rhs_inners;
    double lhs_constant = 0.0;

    decide_inners.reserve(additive_terms.size());
    rhs_inners.reserve(additive_terms.size());

    for (auto &term : additive_terms) {
        if (term.type() == typeid(Numeric)) {
            lhs_constant += double(term);
            continue;
        }
        Symbolic inner;
        bool has_sum = ExtractSumInner(term, inner);
        if (!has_sum) {
            inner = term;
        }
        if (SymbolicContainsDecideVariable(inner, ctx.decide_variables)) {
            decide_inners.push_back(inner);
        } else {
            rhs_inners.push_back(inner);
        }
    }

    if (decide_inners.empty()) {
        return cmp.Copy();
    }

    Symbolic decide_inner_sum = SumSymbolicTerms(decide_inners).simplify();
    auto lhs_inner_expr = BuildFactoredSumExpression(decide_inner_sum, ctx);
    vector<unique_ptr<ParsedExpression>> lhs_sum_args;
    lhs_sum_args.push_back(std::move(lhs_inner_expr));
    auto lhs_sum = make_uniq<FunctionExpression>("sum", std::move(lhs_sum_args));

    double rhs_constant = rhs_num - lhs_constant;
    unique_ptr<ParsedExpression> rhs_expr;
    if (fabs(rhs_constant) > 1e-12 || rhs_inners.empty()) {
        rhs_expr = MakeDoubleConstant(rhs_constant);
    }

    for (auto &inner : rhs_inners) {
        Symbolic neg_inner = (Symbolic(-1.0) * inner).simplify();
        if (SymbolicIsZero(neg_inner)) {
            continue;
        }
        
        unique_ptr<ParsedExpression> term_expr;
        if (neg_inner.type() == typeid(Numeric)) {
            // Pure numeric constant: use constant * count_star() instead of sum(constant)
            // This is mathematically equivalent: sum(c) over n rows = c * n = c * count(*)
            auto const_expr = FromSymbolic(neg_inner, ctx);
            vector<unique_ptr<ParsedExpression>> count_args;
            auto count_star = make_uniq<FunctionExpression>("count_star", std::move(count_args));
            term_expr = MakeOp("*", std::move(const_expr), std::move(count_star));
        } else {
            // Non-constant expression: wrap in sum() as before
            vector<unique_ptr<ParsedExpression>> rhs_args;
            rhs_args.push_back(FromSymbolic(neg_inner, ctx));
            term_expr = make_uniq<FunctionExpression>("sum", std::move(rhs_args));
        }
        
        if (!rhs_expr) {
            rhs_expr = std::move(term_expr);
        } else {
            rhs_expr = MakeOp("+", std::move(rhs_expr), std::move(term_expr));
        }
    }

    if (!rhs_expr) {
        rhs_expr = MakeDoubleConstant(0.0);
    }

    return make_uniq_base<ParsedExpression, ComparisonExpression>(cmp.type, std::move(lhs_sum), std::move(rhs_expr));
}

static unique_ptr<ParsedExpression> NormalizeConstraintsRecursive(const ParsedExpression &expr,
                                                                  const case_insensitive_map_t<idx_t> &decide_variables) {
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::CONJUNCTION: {
            auto &conj = expr.Cast<ConjunctionExpression>();
            vector<unique_ptr<ParsedExpression>> norm_children;
            norm_children.reserve(conj.children.size());
            for (auto &c : conj.children) {
                norm_children.push_back(NormalizeConstraintsRecursive(*c, decide_variables));
            }
            auto result = make_uniq<ConjunctionExpression>(conj.type, std::move(norm_children[0]), std::move(norm_children[1]));
            for (idx_t i = 2; i < norm_children.size(); i++) {
                result = make_uniq<ConjunctionExpression>(conj.type, std::move(result), std::move(norm_children[i]));
            }
            return std::move(result);
        }
        case ExpressionClass::COMPARISON: {
            auto &cmp = expr.Cast<ComparisonExpression>();
            return NormalizeComparisonExpr(cmp, decide_variables);
        }
        case ExpressionClass::FUNCTION: {
            // PackDB: Handle __when_constraint__(constraint, condition)
            auto &func = expr.Cast<FunctionExpression>();
            if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
                // Normalize the inner constraint (child[0]), pass through condition (child[1])
                auto normalized_constraint = NormalizeConstraintsRecursive(*func.children[0], decide_variables);

                // Fix grammar ambiguity: "A AND B WHEN C" parses as "(A AND B) WHEN C"
                // due to a_expr absorbing AND via shift/reduce. The user's intent is
                // "A AND (B WHEN C)" — WHEN binds to the rightmost constraint only.
                // Fix: pull all-but-last AND children out, wrap only the last with WHEN.
                if (normalized_constraint->GetExpressionClass() == ExpressionClass::CONJUNCTION) {
                    auto &conj = normalized_constraint->Cast<ConjunctionExpression>();
                    if (conj.children.size() >= 2) {
                        // Wrap only the last child with WHEN
                        auto cond_copy = func.children[1]->Copy();
                        vector<unique_ptr<ParsedExpression>> when_args;
                        when_args.push_back(std::move(conj.children.back()));
                        when_args.push_back(std::move(cond_copy));
                        auto when_expr = make_uniq<FunctionExpression>(WHEN_CONSTRAINT_TAG, std::move(when_args));
                        when_expr->is_operator = true;

                        // Rebuild: unwrapped children AND WHEN(last, condition)
                        conj.children.pop_back();
                        conj.children.push_back(std::move(when_expr));
                        return std::move(normalized_constraint);
                    }
                }

                auto condition_copy = func.children[1]->Copy();
                vector<unique_ptr<ParsedExpression>> args;
                args.push_back(std::move(normalized_constraint));
                args.push_back(std::move(condition_copy));
                auto result = make_uniq<FunctionExpression>(WHEN_CONSTRAINT_TAG, std::move(args));
                result->is_operator = true;
                return std::move(result);
            }
            if (func.is_operator && func.function_name == PER_CONSTRAINT_TAG) {
                // Normalize the inner constraint (child[0]), pass through PER columns (children[1..N])
                auto normalized_constraint = NormalizeConstraintsRecursive(*func.children[0], decide_variables);
                vector<unique_ptr<ParsedExpression>> args;
                args.push_back(std::move(normalized_constraint));
                for (idx_t i = 1; i < func.children.size(); i++) {
                    args.push_back(func.children[i]->Copy());
                }
                auto result = make_uniq<FunctionExpression>(PER_CONSTRAINT_TAG, std::move(args));
                result->is_operator = true;
                return std::move(result);
            }
            return expr.Copy();
        }
        default:
            return expr.Copy();
    }
}

unique_ptr<ParsedExpression> NormalizeDecideConstraints(const ParsedExpression &expr,
                                                        const case_insensitive_map_t<idx_t> &decide_variables) {
    return NormalizeConstraintsRecursive(expr, decide_variables);
}

static bool ExprContainsDecideVar(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
    if (IsDecideVariable(expr, variables)) return true;
    bool found = false;
    ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
        if (!found && ExprContainsDecideVar(child, variables)) {
            found = true;
        }
    });
    return found;
}

static bool SumInnerIsQuadratic(const ParsedExpression &inner,
                                const case_insensitive_map_t<idx_t> &decide_variables) {
    if (inner.GetExpressionClass() != ExpressionClass::FUNCTION) return false;
    auto &func = inner.Cast<FunctionExpression>();
    string name_lower = StringUtil::Lower(func.function_name);

    // POWER(expr, 2), POW(expr, 2)
    if (!func.is_operator && (name_lower == "power" || name_lower == "pow")) {
        if (func.children.size() == 2 &&
            func.children[1]->GetExpressionClass() == ExpressionClass::CONSTANT) {
            double val;
            if (IsNumericConstant(*func.children[1], val) && val == 2.0) {
                return ExprContainsDecideVar(*func.children[0], decide_variables);
            }
        }
        return false;
    }

    // expr ** 2
    if (func.is_operator && func.function_name == "**") {
        if (func.children.size() == 2 &&
            func.children[1]->GetExpressionClass() == ExpressionClass::CONSTANT) {
            double val;
            if (IsNumericConstant(*func.children[1], val) && val == 2.0) {
                return ExprContainsDecideVar(*func.children[0], decide_variables);
            }
        }
        return false;
    }

    // (expr) * (expr) where both sides are identical and contain a DECIDE variable
    if (func.is_operator && func.function_name == "*") {
        if (func.children.size() == 2 &&
            func.children[0]->ToString() == func.children[1]->ToString()) {
            return ExprContainsDecideVar(*func.children[0], decide_variables);
        }
    }

    return false;
}

unique_ptr<ParsedExpression> NormalizeDecideObjective(const ParsedExpression &expr,
                                                      const case_insensitive_map_t<idx_t> &decide_variables) {
    // Expect SUM(inner), possibly wrapped in WHEN or PER
    if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
        return expr.Copy();
    }
    auto &f = expr.Cast<FunctionExpression>();
    // PackDB: Handle WHEN wrapper — normalize inner objective, pass through condition
    if (f.is_operator && f.function_name == WHEN_CONSTRAINT_TAG) {
        auto normalized = NormalizeDecideObjective(*f.children[0], decide_variables);
        auto cond = f.children[1]->Copy();
        vector<unique_ptr<ParsedExpression>> args;
        args.push_back(std::move(normalized));
        args.push_back(std::move(cond));
        auto result = make_uniq<FunctionExpression>(WHEN_CONSTRAINT_TAG, std::move(args));
        result->is_operator = true;
        return std::move(result);
    }
    // PackDB: Handle PER wrapper — normalize inner objective, pass through PER columns
    if (f.is_operator && f.function_name == PER_CONSTRAINT_TAG) {
        auto normalized = NormalizeDecideObjective(*f.children[0], decide_variables);
        vector<unique_ptr<ParsedExpression>> args;
        args.push_back(std::move(normalized));
        for (idx_t i = 1; i < f.children.size(); i++) {
            args.push_back(f.children[i]->Copy());
        }
        auto result = make_uniq<FunctionExpression>(PER_CONSTRAINT_TAG, std::move(args));
        result->is_operator = true;
        return std::move(result);
    }
    if (StringUtil::Lower(f.function_name) != "sum" || f.children.empty()) {
        return expr.Copy();
    }

    // Skip normalization for quadratic objectives — the symbolic expansion
    // would destroy the POWER(expr, 2) structure that the QP pipeline needs.
    if (SumInnerIsQuadratic(*f.children[0], decide_variables)) {
        return expr.Copy();
    }

    // Convert inner to Symbolic and simplify
    SymbolicTranslationContext ctx(decide_variables);
    Symbolic inner_sym = ToSymbolicObj(*f.children[0], ctx).expand().simplify();

    auto factored_terms = CollectFactoredTerms(inner_sym, ctx.decide_variables);
    vector<FactoredTerm> decide_terms;
    decide_terms.reserve(factored_terms.size());
    for (auto &term : factored_terms) {
        if (!SymbolicContainsDecideVariable(term.decide_part, ctx.decide_variables)) {
            continue;
        }
        decide_terms.push_back(term);
    }

    Symbolic combined_sym;
    bool has_terms = !decide_terms.empty();
    if (has_terms) {
        bool first = true;
        for (auto &term : decide_terms) {
            Symbolic combined(term.decide_part);
            combined = combined * term.coefficient;
            if (first) {
                combined_sym = combined;
                first = false;
            } else {
                combined_sym = combined_sym + combined;
            }
        }
    } else {
        combined_sym = Symbolic(0.0);
    }

    combined_sym = combined_sym.expand().simplify();
    auto new_inner = BuildFactoredSumExpression(combined_sym, ctx);
    vector<unique_ptr<ParsedExpression>> args;
    args.push_back(std::move(new_inner));
    return make_uniq_base<ParsedExpression, FunctionExpression>("sum", std::move(args));
}

//===--------------------------------------------------------------------===//
// DOT graph export for ParsedExpression
//===--------------------------------------------------------------------===//

static void DotEscape(string &s) {
    for (auto &ch : s) {
        if (ch == '"') ch = '\'';
    }
}

static void ExpressionToDotImpl(const ParsedExpression &expr, std::stringstream &ss, idx_t &next_id, idx_t parent_id) {
    idx_t my_id = next_id++;
    string label = EnumUtil::ToString(expr.GetExpressionClass());
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::FUNCTION: {
            auto &f = expr.Cast<FunctionExpression>();
            label = string("FUNCTION ") + f.function_name + (f.is_operator ? " (op)" : "");
            break;
        }
        case ExpressionClass::COLUMN_REF: {
            auto &c = expr.Cast<ColumnRefExpression>();
            label = string("COLUMN ") + c.GetColumnName();
            break;
        }
        case ExpressionClass::CONSTANT: {
            auto &c = expr.Cast<ConstantExpression>();
            label = string("CONST ") + c.value.ToString();
            break;
        }
        case ExpressionClass::COMPARISON: {
            auto &c = expr.Cast<ComparisonExpression>();
            label = string("COMP ") + ExpressionTypeToString(c.type);
            break;
        }
        case ExpressionClass::CONJUNCTION: {
            auto &c = expr.Cast<ConjunctionExpression>();
            label = string("CONJ ") + ExpressionTypeToString(c.type);
            break;
        }
        case ExpressionClass::OPERATOR: {
            auto &o = expr.Cast<OperatorExpression>();
            label = string("OPER ") + ExpressionTypeToString(o.type);
            break;
        }
        case ExpressionClass::CAST: {
            auto &c = expr.Cast<CastExpression>();
            label = string("CAST ") + c.cast_type.ToString();
            break;
        }
        case ExpressionClass::BETWEEN: {
            label = "BETWEEN";
            break;
        }
        default:
            break;
    }
    DotEscape(label);
    ss << "  n" << my_id << " [label=\"" << label << "\"];\n";
    if (parent_id != (idx_t)-1) {
        ss << "  n" << parent_id << " -> n" << my_id << ";\n";
    }

    switch (expr.GetExpressionClass()) {
        case ExpressionClass::FUNCTION: {
            auto &f = expr.Cast<FunctionExpression>();
            for (auto &ch : f.children) ExpressionToDotImpl(*ch, ss, next_id, my_id);
            if (f.filter) ExpressionToDotImpl(*f.filter, ss, next_id, my_id);
            break;
        }
        case ExpressionClass::COMPARISON: {
            auto &c = expr.Cast<ComparisonExpression>();
            ExpressionToDotImpl(*c.left, ss, next_id, my_id);
            ExpressionToDotImpl(*c.right, ss, next_id, my_id);
            break;
        }
        case ExpressionClass::CONJUNCTION: {
            auto &c = expr.Cast<ConjunctionExpression>();
            for (auto &ch : c.children) ExpressionToDotImpl(*ch, ss, next_id, my_id);
            break;
        }
        case ExpressionClass::OPERATOR: {
            auto &o = expr.Cast<OperatorExpression>();
            for (auto &ch : o.children) ExpressionToDotImpl(*ch, ss, next_id, my_id);
            break;
        }
        case ExpressionClass::CAST: {
            auto &c = expr.Cast<CastExpression>();
            ExpressionToDotImpl(*c.child, ss, next_id, my_id);
            break;
        }
        case ExpressionClass::BETWEEN: {
            auto &b = expr.Cast<BetweenExpression>();
            ExpressionToDotImpl(*b.input, ss, next_id, my_id);
            ExpressionToDotImpl(*b.lower, ss, next_id, my_id);
            ExpressionToDotImpl(*b.upper, ss, next_id, my_id);
            break;
        }
        default:
            break;
    }
}

string ExpressionToDot(const ParsedExpression &expr) {
    std::stringstream ss;
    ss << "digraph ParsedExpression {\n";
    ss << "  node [shape=box, fontsize=10];\n";
    idx_t next_id = 0;
    ExpressionToDotImpl(expr, ss, next_id, (idx_t)-1);
    ss << "}\n";
    return ss.str();
}

} // namespace duckdb
