#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include <cmath>
#include <cstdlib>
#include "duckdb/common/profiler.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include "duckdb/packdb/utility/debug.hpp"
#include "duckdb/packdb/ilp_solver.hpp"
#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Expression Transform Helpers
//===--------------------------------------------------------------------===//
// These static functions replace BoundColumnRefExpression nodes with
// BoundReferenceExpression nodes so that DuckDB's ExpressionExecutor can
// evaluate expressions against data chunks (which use positional indices).

//! Transform a coefficient/value expression for ExpressionExecutor.
//! Handles: BOUND_COLUMN_REF, BOUND_FUNCTION, BOUND_CAST, BOUND_AGGREGATE (count_star→constant),
//! BOUND_COMPARISON, BOUND_CONJUNCTION, BOUND_OPERATOR. Falls back to Copy() for others.
static unique_ptr<Expression> TransformToChunkExpression(const Expression &expr, ClientContext &context,
                                                         idx_t num_rows = 0) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		vector<unique_ptr<Expression>> new_children;
		for (auto &child : func.children) {
			new_children.push_back(TransformToChunkExpression(*child, context, num_rows));
		}
		unique_ptr<FunctionData> new_bind_info;
		if (func.bind_info) {
			new_bind_info = func.bind_info->Copy();
		}
		return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function,
		                                                           std::move(new_children), std::move(new_bind_info));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		auto transformed_child = TransformToChunkExpression(*cast.child, context, num_rows);
		return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type, cast.try_cast);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = expr.Cast<BoundAggregateExpression>();
		if (agg.function.name == "count_star") {
			return make_uniq_base<Expression, BoundConstantExpression>(Value::BIGINT(num_rows));
		}
		throw InternalException("Unsupported aggregate '%s' in constraint RHS. "
		                        "Only count_star() is supported.", agg.function.name);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		auto left = TransformToChunkExpression(*comp.left, context, num_rows);
		auto right = TransformToChunkExpression(*comp.right, context, num_rows);
		return make_uniq_base<Expression, BoundComparisonExpression>(comp.type, std::move(left), std::move(right));
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
		for (auto &child : conj.children) {
			result->children.push_back(TransformToChunkExpression(*child, context, num_rows));
		}
		return std::move(result);
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		auto result = make_uniq<BoundOperatorExpression>(op_expr.type, op_expr.return_type);
		for (auto &child : op_expr.children) {
			result->children.push_back(TransformToChunkExpression(*child, context, num_rows));
		}
		return std::move(result);
	} else {
		return expr.Copy();
	}
}

//===--------------------------------------------------------------------===//
// Expression Analysis Helper Functions
//===--------------------------------------------------------------------===//

idx_t PhysicalDecide::FindDecideVariable(const Expression &expr) const {
    // Base case: check if this is a column reference to a DECIDE variable
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        for (idx_t i = 0; i < decide_variables.size(); i++) {
            auto &decide_var = decide_variables[i]->Cast<BoundColumnRefExpression>();
            if (colref.binding == decide_var.binding) {
                return i;
            }
        }
    }

    // Recursive case: search in children
    idx_t result = DConstants::INVALID_INDEX;
    ExpressionIterator::EnumerateChildren(const_cast<Expression&>(expr),
        [&](unique_ptr<Expression> &child) {
            if (result == DConstants::INVALID_INDEX && child) {
                result = FindDecideVariable(*child);
            }
        });
    return result;
}

bool PhysicalDecide::ContainsVariable(const Expression &expr, idx_t var_idx) const {
    // Check if this expression is the variable we're looking for
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        auto &decide_var = decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
        return colref.binding == decide_var.binding;
    }

    // Recursively check children
    bool found = false;
    ExpressionIterator::EnumerateChildren(const_cast<Expression&>(expr),
        [&](unique_ptr<Expression> &child) {
            if (!found && child && ContainsVariable(*child, var_idx)) {
                found = true;
            }
        });
    return found;
}

bool PhysicalDecide::IsLinearInDecideVars(const Expression &expr) const {
    // Column refs and constants: a decide-var col-ref contributes degree 1;
    // non-decide col-refs and constants contribute degree 0. Both are linear.
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF ||
        expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT ||
        expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
        return true;
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        return IsLinearInDecideVars(*expr.Cast<BoundCastExpression>().child);
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        string fname = StringUtil::Lower(func.function.name);

        // Additive operators preserve linearity iff every child is linear.
        if (fname == "+" || fname == "-") {
            for (auto &child : func.children) {
                if (!IsLinearInDecideVars(*child)) {
                    return false;
                }
            }
            return true;
        }

        // Multiplication is linear iff at most one factor contains a decide
        // variable and every factor is itself linear. Two var-carrying factors
        // (e.g. x * y, x * POWER(y,2)) push the product to degree ≥ 2.
        if (fname == "*") {
            idx_t factors_with_vars = 0;
            for (auto &child : func.children) {
                if (!IsLinearInDecideVars(*child)) {
                    return false;
                }
                if (FindDecideVariable(*child) != DConstants::INVALID_INDEX) {
                    factors_with_vars++;
                }
            }
            return factors_with_vars <= 1;
        }

        // Any other function (POWER, SIN, ABS, ...) is linear only when none
        // of its arguments reference a decide variable (it is a pure data
        // expression evaluated at runtime into a coefficient).
        return FindDecideVariable(expr) == DConstants::INVALID_INDEX;
    }

    // Unknown expression classes: linear only if they contain no decide var.
    return FindDecideVariable(expr) == DConstants::INVALID_INDEX;
}

unique_ptr<Expression> PhysicalDecide::ExtractCoefficientWithoutVariable(const Expression &expr, idx_t var_idx) const {
    // If this IS the variable itself, return constant 1
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        auto &decide_var = decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
        if (colref.binding == decide_var.binding) {
            return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
        }
    }

    // If it's a multiplication, filter out children containing the variable
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        if (func.function.name == "*") {
            vector<unique_ptr<Expression>> filtered_children;
            for (auto &child : func.children) {
                if (!ContainsVariable(*child, var_idx)) {
                    filtered_children.push_back(child->Copy());
                }
            }

            if (filtered_children.empty()) {
                return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
            }
            if (filtered_children.size() == 1) {
                return std::move(filtered_children[0]);
            }

            // Rebuild multiplication with remaining children
            return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function,
                                                     std::move(filtered_children), nullptr);
        }
    }

    // If it's a cast, recurse into child
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        return ExtractCoefficientWithoutVariable(*cast.child, var_idx);
    }

    // Otherwise, return a copy of the entire expression (no variable in it)
    return expr.Copy();
}

//! Result of DetectQuadraticPattern. `inner_linear_expr` is a non-owning
//! pointer into the tree rooted at the caller's expression (valid only
//! while that tree is alive). `sign` carries the scalar multiplier from
//! negation and constant-times-quadratic patterns (e.g. `-POWER(x,2)` → -1,
//! `(-2)*POWER` → -2). When `inner_linear_expr == nullptr`, no pattern
//! matched. Defined here (not in the header) so the lifetime contract stays
//! internal to this translation unit.
struct PhysicalDecide::QuadraticPattern {
    const Expression *inner_linear_expr = nullptr;
    double sign = 1.0;
};

PhysicalDecide::QuadraticPattern PhysicalDecide::DetectQuadraticPattern(const Expression &expr) const {
    // Unwrap any cast wrappers on the incoming expression.
    const Expression *cur = &expr;
    while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        cur = cur->Cast<BoundCastExpression>().child.get();
    }
    if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
        return {};
    }
    auto &func = cur->Cast<BoundFunctionExpression>();
    string fname = StringUtil::Lower(func.function.name);

    // Fast path: nothing below this point can match on names outside this set.
    // Without the gate every recursive additive (`+`) node in the objective
    // tree would pay for the self-product `ToString() == ToString()` compare,
    // which is O(subtree-size) — turning the walker into O(n^2) on deep sums.
    if (fname != "-" && fname != "*" && fname != "power" && fname != "pow" && fname != "**") {
        return {};
    }

    // -(quadratic)
    if (fname == "-" && func.children.size() == 1) {
        auto inner = DetectQuadraticPattern(*func.children[0]);
        if (inner.inner_linear_expr) {
            return {inner.inner_linear_expr, -inner.sign};
        }
    }

    // K * quadratic or quadratic * K (constant on either side)
    if (fname == "*" && func.children.size() == 2) {
        for (idx_t side = 0; side < 2; side++) {
            const Expression *maybe_const = func.children[side].get();
            while (maybe_const->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                maybe_const = maybe_const->Cast<BoundCastExpression>().child.get();
            }
            if (maybe_const->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                double cval = maybe_const->Cast<BoundConstantExpression>()
                                  .value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                if (cval != 0.0) {
                    auto inner = DetectQuadraticPattern(*func.children[1 - side]);
                    if (inner.inner_linear_expr) {
                        return {inner.inner_linear_expr, cval * inner.sign};
                    }
                }
            }
        }
    }

    // POWER / POW / **  with literal exponent 2
    if ((fname == "power" || fname == "pow" || fname == "**") && func.children.size() == 2) {
        const Expression *exp_expr = func.children[1].get();
        while (exp_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            exp_expr = exp_expr->Cast<BoundCastExpression>().child.get();
        }
        if (exp_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
            double exponent = exp_expr->Cast<BoundConstantExpression>()
                                  .value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
            if (exponent == 2.0) {
                const Expression *inner = func.children[0].get();
                while (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    inner = inner->Cast<BoundCastExpression>().child.get();
                }
                if (FindDecideVariable(*inner) != DConstants::INVALID_INDEX) {
                    // Shape matches POWER(expr, 2); reject expr that is itself
                    // degree > 1 in decide vars (e.g. POWER(POWER(x,2), 2) =
                    // x^4, POWER(x*y, 2) = x^2 y^2) rather than silently
                    // emitting an x^2-shaped Q term.
                    if (!IsLinearInDecideVars(*inner)) {
                        throw InvalidInputException(
                            "DECIDE objective/constraint contains a non-linear expression "
                            "inside POWER(..., 2) (total degree > 2 in decision variables). "
                            "Only POWER(linear_expr, 2) is supported; rewrite the expression "
                            "or combine it into a single quadratic group.");
                    }
                    return {inner, 1.0};
                }
            }
        }
    }

    // (expr) * (expr) with identical children containing a DECIDE variable
    if (fname == "*" && func.children.size() == 2 &&
        func.children[0]->ToString() == func.children[1]->ToString() &&
        FindDecideVariable(*func.children[0]) != DConstants::INVALID_INDEX) {
        const Expression *inner = func.children[0].get();
        while (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            inner = inner->Cast<BoundCastExpression>().child.get();
        }
        // Identical-child self-product matches `(expr)*(expr)`; the inner
        // must be linear in decide vars or the product is degree > 2 (e.g.
        // POWER(x,2) * POWER(x,2) = x^4, (x*y) * (x*y) = x^2 y^2).
        if (!IsLinearInDecideVars(*inner)) {
            throw InvalidInputException(
                "DECIDE objective/constraint contains a self-product of a non-linear "
                "expression (e.g. POWER(x, 2) * POWER(x, 2) or (x*y) * (x*y)), total "
                "degree > 2 in decision variables. Only (linear_expr) * (linear_expr) "
                "is supported as a quadratic pattern.");
        }
        return {inner, 1.0};
    }

    return {};
}

void PhysicalDecide::ExtractTerms(const Expression &expr, vector<Term> &out_terms) const {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();

        // Addition: recursively process all children
        if (func.function.name == "+") {
            for (auto &child : func.children) {
                ExtractTerms(*child, out_terms);
            }
            return;
        }

        // Subtraction: first child positive, second child negated
        if (func.function.name == "-" && func.children.size() == 2) {
            ExtractTerms(*func.children[0], out_terms);
            idx_t before = out_terms.size();
            ExtractTerms(*func.children[1], out_terms);
            for (idx_t i = before; i < out_terms.size(); i++) {
                out_terms[i].sign *= -1;
            }
            return;
        }

        // Multiplication: extract variable and coefficient
        if (func.function.name == "*") {
            idx_t var_idx = FindDecideVariable(func);

            if (var_idx == DConstants::INVALID_INDEX) {
                // No variable found - this is a constant term
                out_terms.push_back(Term{DConstants::INVALID_INDEX, func.Copy()});
            } else {
                // Variable found - extract coefficient
                auto coef = ExtractCoefficientWithoutVariable(func, var_idx);
                out_terms.push_back(Term{var_idx, std::move(coef)});
            }
            return;
        }
    }

    // Handle casts
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        ExtractTerms(*cast.child, out_terms);
        return;
    }

    // Base case: constant or simple column reference
    idx_t var_idx = FindDecideVariable(expr);
    if (var_idx == DConstants::INVALID_INDEX) {
        // Constant term
        out_terms.push_back(Term{DConstants::INVALID_INDEX, expr.Copy()});
    } else {
        // Just a variable (coefficient = 1)
        out_terms.push_back(Term{var_idx,
            make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))});
    }
}

static bool BoundExpressionContainsAggregate(const Expression &expr) {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
        return true;
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        return BoundExpressionContainsAggregate(*cast.child);
    }
    bool found = false;
    ExpressionIterator::EnumerateChildren(const_cast<Expression &>(expr), [&](unique_ptr<Expression> &child) {
        if (!found && child && BoundExpressionContainsAggregate(*child)) {
            found = true;
        }
    });
    return found;
}

//===--------------------------------------------------------------------===//
// Constructor
//===--------------------------------------------------------------------===//

PhysicalDecide::PhysicalDecide(vector<LogicalType> types, idx_t estimated_cardinality, 
                    unique_ptr<PhysicalOperator> child, idx_t decide_index, 
                    vector<unique_ptr<Expression>> decide_variables,
                    unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                    unique_ptr<Expression> decide_objective)
    : PhysicalOperator(PhysicalOperatorType::DECIDE, std::move(types), estimated_cardinality)
    , decide_index(decide_index)
    , decide_variables(std::move(decide_variables))
    , decide_constraints(std::move(decide_constraints))
    , decide_sense(decide_sense)
    , decide_objective(std::move(decide_objective)) {
    children.push_back(std::move(child));
}

//===--------------------------------------------------------------------===//
// EXPLAIN Support
//===--------------------------------------------------------------------===//

string PhysicalDecide::GetName() const {
	return "DECIDE";
}

static void CollectConstraintStringsPhysical(const Expression &expr, vector<string> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
			string per_suffix = IsPerStrictTag(conj.alias) ? " PER STRICT " : " PER ";
			for (idx_t i = 1; i < conj.children.size(); i++) {
				if (i > 1) {
					per_suffix += ", ";
				}
				per_suffix += conj.children[i]->GetName();
			}
			vector<string> inner;
			CollectConstraintStringsPhysical(*conj.children[0], inner);
			for (auto &s : inner) {
				out.push_back(s + per_suffix);
			}
			return;
		}
		if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
			string when_suffix = " WHEN " + conj.children[1]->GetName();
			vector<string> inner;
			CollectConstraintStringsPhysical(*conj.children[0], inner);
			for (auto &s : inner) {
				out.push_back(s + when_suffix);
			}
			return;
		}
		for (auto &child : conj.children) {
			CollectConstraintStringsPhysical(*child, out);
		}
		return;
	}
	out.push_back(expr.GetName());
}

InsertionOrderPreservingMap<string> PhysicalDecide::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;

	string vars_info;
	idx_t user_var_count = decide_variables.size() - num_auxiliary_vars;
	for (idx_t i = 0; i < user_var_count; i++) {
		if (i > 0) {
			vars_info += "\n";
		}
		vars_info += decide_variables[i]->GetName();
	}
	result["Variables"] = vars_info;

	if (decide_objective) {
		string obj_info = (decide_sense == DecideSense::MAXIMIZE) ? "MAXIMIZE " : "MINIMIZE ";
		obj_info += decide_objective->GetName();
		result["Objective"] = obj_info;
	} else {
		result["Objective"] = "FEASIBILITY";
	}

	if (decide_constraints) {
		vector<string> constraint_strs;
		CollectConstraintStringsPhysical(*decide_constraints, constraint_strs);
		string constraints_info;
		for (idx_t i = 0; i < constraint_strs.size(); i++) {
			if (i > 0) {
				constraints_info += "\n";
			}
			constraints_info += constraint_strs[i];
		}
		result["Constraints"] = constraints_info;
	}

	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

//===--------------------------------------------------------------------===//
// Multi-variable per-row constraint helpers
//===--------------------------------------------------------------------===//

//! Collect DECIDE variable references from a bound expression, tracking sign
//! through subtraction operators. Used for multi-variable per-row constraints.
struct ExprVarRef {
    idx_t var_idx;
    int sign; // +1 or -1
};

static void CollectDecideVarRefs(const Expression &expr, int sign,
                                  vector<ExprVarRef> &refs,
                                  const PhysicalDecide &op) {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        idx_t var_idx = op.FindDecideVariable(expr);
        if (var_idx != DConstants::INVALID_INDEX) {
            refs.push_back({var_idx, sign});
        }
        return;
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        if (func.function.name == "-" && func.children.size() == 2) {
            CollectDecideVarRefs(*func.children[0], sign, refs, op);
            CollectDecideVarRefs(*func.children[1], -sign, refs, op);
            return;
        }
        if (func.function.name == "+" && func.children.size() == 2) {
            CollectDecideVarRefs(*func.children[0], sign, refs, op);
            CollectDecideVarRefs(*func.children[1], sign, refs, op);
            return;
        }
        if (func.function.name == "*" && func.children.size() == 2) {
            // Multiplication: descend into both children to find decide variables.
            // Sign propagates unchanged — * doesn't flip algebraic sign, it changes
            // the coefficient magnitude (handled separately by FindVarCoefficient).
            CollectDecideVarRefs(*func.children[0], sign, refs, op);
            CollectDecideVarRefs(*func.children[1], sign, refs, op);
            return;
        }
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        CollectDecideVarRefs(*cast.child, sign, refs, op);
        return;
    }
    // Constants, data columns, etc.: no DECIDE vars
}

//! Replace all DECIDE variable references in a bound expression with constant 0.
//! Returns a data-only expression that can be evaluated per-row from the input chunk.
static unique_ptr<Expression> StripDecideVars(const Expression &expr, const PhysicalDecide &op) {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        idx_t var_idx = op.FindDecideVariable(expr);
        if (var_idx != DConstants::INVALID_INDEX) {
            return make_uniq_base<Expression, BoundConstantExpression>(Value::DOUBLE(0.0));
        }
        return expr.Copy();
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        vector<unique_ptr<Expression>> new_children;
        for (auto &child : func.children) {
            new_children.push_back(StripDecideVars(*child, op));
        }
        unique_ptr<FunctionData> new_bind_info;
        if (func.bind_info) {
            new_bind_info = func.bind_info->Copy();
        }
        return make_uniq_base<Expression, BoundFunctionExpression>(
            func.return_type, func.function, std::move(new_children), std::move(new_bind_info));
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        auto new_child = StripDecideVars(*cast.child, op);
        // Recreate the cast
        return make_uniq_base<Expression, BoundCastExpression>(
            std::move(new_child), cast.return_type, cast.bound_cast.Copy(), cast.try_cast);
    }
    return expr.Copy();
}

//! Walk through +/- nodes to find the sub-expression containing a specific decide
//! variable, then extract its coefficient using ExtractCoefficientWithoutVariable.
//! Returns the unsigned coefficient (sign is tracked separately by CollectDecideVarRefs).
static unique_ptr<Expression> FindVarCoefficient(
    const Expression &expr, idx_t var_idx, const PhysicalDecide &op) {
    if (!op.ContainsVariable(expr, var_idx)) {
        return nullptr;
    }
    // Bare variable reference: coefficient is 1
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        if (op.FindDecideVariable(expr) == var_idx) {
            return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
        }
        return nullptr;
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        // Multiplication: this is the coefficient node — extract the non-variable part
        if (func.function.name == "*") {
            return op.ExtractCoefficientWithoutVariable(expr, var_idx);
        }
        // Addition/subtraction: recurse into the child that contains the variable
        if ((func.function.name == "+" || func.function.name == "-") && func.children.size() == 2) {
            for (auto &child : func.children) {
                auto result = FindVarCoefficient(*child, var_idx, op);
                if (result) {
                    return result;
                }
            }
        }
    }
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        return FindVarCoefficient(*cast.child, var_idx, op);
    }
    return nullptr;
}

//===--------------------------------------------------------------------===//
// Sink (Collecting Data)
//===--------------------------------------------------------------------===//
class DecideGlobalSinkState : public GlobalSinkState {
public:
    explicit DecideGlobalSinkState(ClientContext &context, const PhysicalDecide &op)
        : data(context, op.children[0]->GetTypes()), op(op) {
        // Analyze constraints and objective using new visitor-based approach
        AnalyzeConstraint(op.decide_constraints);
        if (op.decide_objective) {
            AnalyzeObjective(op.decide_objective);
        }

        // Minimal: keep constructor lean; detailed solver output comes from HiGHS
    }

    static void ApplyAggregateMetadata(vector<Term> &terms, idx_t begin, const BoundAggregateExpression &agg) {
        bool is_avg = (agg.alias == AVG_REWRITE_TAG);
        for (idx_t i = begin; i < terms.size(); i++) {
            if (agg.filter) {
                terms[i].filter = agg.filter->Copy();
            }
            terms[i].avg_scale = is_avg;
        }
    }

    void ExtractAggregateConstraintTerms(const Expression &expr, DecideConstraint &constraint, int sign) {
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            auto &cast = expr.Cast<BoundCastExpression>();
            ExtractAggregateConstraintTerms(*cast.child, constraint, sign);
            return;
        }
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
            auto &func = expr.Cast<BoundFunctionExpression>();
            if (func.function.name == "+") {
                for (auto &child : func.children) {
                    ExtractAggregateConstraintTerms(*child, constraint, sign);
                }
                return;
            }
            if (func.function.name == "-" && func.children.size() == 2) {
                ExtractAggregateConstraintTerms(*func.children[0], constraint, sign);
                ExtractAggregateConstraintTerms(*func.children[1], constraint, -sign);
                return;
            }
            if (func.function.name == "-" && func.children.size() == 1) {
                ExtractAggregateConstraintTerms(*func.children[0], constraint, -sign);
                return;
            }
        }
        if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
            throw InternalException("DECIDE aggregate constraint LHS contains a non-aggregate term: %s",
                                    expr.ToString());
        }

        auto &agg = expr.Cast<BoundAggregateExpression>();
        auto agg_name = StringUtil::Lower(agg.function.name);
        if (agg_name != "sum") {
            throw InternalException("DECIDE optimizer should rewrite aggregate '%s' to SUM before execution",
                                    agg.function.name);
        }
        bool is_avg = (agg.alias == AVG_REWRITE_TAG);

        idx_t linear_before = constraint.lhs_terms.size();
        idx_t bilinear_before = constraint.bilinear_terms.size();
        idx_t quadratic_before = constraint.quadratic_groups.size();
        ExtractConstraintTerms(*agg.children[0], constraint, sign);
        ApplyAggregateMetadata(constraint.lhs_terms, linear_before, agg);
        for (idx_t i = bilinear_before; i < constraint.bilinear_terms.size(); i++) {
            if (agg.filter) {
                constraint.bilinear_terms[i].filter = agg.filter->Copy();
            }
            constraint.bilinear_terms[i].avg_scale = is_avg;
        }
        for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
            if (agg.filter) {
                constraint.quadratic_groups[i].filter = agg.filter->Copy();
            }
            constraint.quadratic_groups[i].avg_scale = is_avg;
        }

        if (agg.alias.size() > strlen(MINMAX_INDICATOR_TAG_PREFIX) + 2 &&
            agg.alias.substr(0, strlen(MINMAX_INDICATOR_TAG_PREFIX)) == MINMAX_INDICATOR_TAG_PREFIX) {
            auto payload = agg.alias.substr(strlen(MINMAX_INDICATOR_TAG_PREFIX));
            payload = payload.substr(0, payload.size() - 2);
            auto sep = payload.find('_');
            constraint.minmax_indicator_idx = std::stoull(payload.substr(0, sep));
            constraint.minmax_agg_type = payload.substr(sep + 1);
        }
    }

    void AnalyzeConstraint(const unique_ptr<Expression>& expr_ptr,
                           unique_ptr<Expression> when_condition = nullptr,
                           vector<unique_ptr<Expression>> per_columns = {},
                           bool per_strict = false) {
        auto &expr = *expr_ptr;
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                // PackDB: PER [STRICT] wrapper — outermost layer
                if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
                    // child[0] = the constraint (possibly WHEN-wrapped)
                    // children[1..N] = the PER column expressions
                    vector<unique_ptr<Expression>> per_cols;
                    bool strict = IsPerStrictTag(conj.alias);
                    for (idx_t i = 1; i < conj.children.size(); i++) {
                        per_cols.push_back(conj.children[i]->Copy());
                    }
                    AnalyzeConstraint(conj.children[0], std::move(when_condition),
                                      std::move(per_cols), strict);
                    break;
                }
                // PackDB: Check if this is a WHEN constraint wrapper
                if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
                    // child[0] = the actual constraint, child[1] = the WHEN condition
                    AnalyzeConstraint(conj.children[0], conj.children[1]->Copy(),
                                      std::move(per_columns), per_strict);
                    break;
                }
                // Regular conjunction: recursively analyze each child
                for (auto &child : conj.children) {
                    AnalyzeConstraint(child);
                }
                break;
            }

            case ExpressionClass::BOUND_COMPARISON: {
                auto &comp = expr.Cast<BoundComparisonExpression>();

                auto constraint = make_uniq<DecideConstraint>();
                constraint->comparison_type = comp.type;
                constraint->rhs_expr = comp.right->Copy();

                // Parse not-equal indicator tag if present
                if (comp.alias.size() > strlen(NE_INDICATOR_TAG_PREFIX) + 2 &&
                    comp.alias.substr(0, strlen(NE_INDICATOR_TAG_PREFIX)) == NE_INDICATOR_TAG_PREFIX) {
                    auto payload = comp.alias.substr(strlen(NE_INDICATOR_TAG_PREFIX));
                    payload = payload.substr(0, payload.size() - 2);  // strip trailing "__"
                    constraint->ne_indicator_idx = std::stoull(payload);
                }

                // PackDB: Store WHEN condition and PER columns if present
                if (when_condition) {
                    constraint->when_condition = std::move(when_condition);
                }
                if (!per_columns.empty()) {
                    constraint->per_columns = std::move(per_columns);
                }
                constraint->per_strict = per_strict;

                // Extract terms from LHS
                Expression *lhs = comp.left.get();
                while (lhs->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    lhs = lhs->Cast<BoundCastExpression>().child.get();
                }

                if (BoundExpressionContainsAggregate(*lhs)) {
                    // Aggregate constraint. Handles both legacy single aggregates and
                    // additive aggregate expressions with aggregate-local WHEN filters.
                    constraint->lhs_is_aggregate = true;
                    ExtractAggregateConstraintTerms(*lhs, *constraint, 1);
                } else {
                    // Per-row constraint (e.g., x <= 5, or multi-variable: d >= x - c)
                    constraint->lhs_is_aggregate = false;

                    // Check if RHS contains DECIDE variables (multi-variable constraint)
                    vector<ExprVarRef> rhs_refs;
                    CollectDecideVarRefs(*comp.right, +1, rhs_refs, op);

                    if (!rhs_refs.empty()) {
                        // Multi-variable per-row constraint (e.g., ABS linearization: d >= x - c)
                        // Collect LHS DECIDE vars
                        vector<ExprVarRef> lhs_refs;
                        CollectDecideVarRefs(*lhs, +1, lhs_refs, op);

                        // LHS vars: extract row-varying coefficients, keep sign
                        for (auto &ref : lhs_refs) {
                            auto coef = FindVarCoefficient(*lhs, ref.var_idx, op);
                            if (!coef) {
                                coef = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
                            }
                            constraint->lhs_terms.push_back(Term{ref.var_idx, std::move(coef), ref.sign});
                        }
                        // RHS vars: extract row-varying coefficients, negate sign (moving to LHS)
                        for (auto &ref : rhs_refs) {
                            auto coef = FindVarCoefficient(*comp.right, ref.var_idx, op);
                            if (!coef) {
                                coef = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
                            }
                            constraint->lhs_terms.push_back(Term{ref.var_idx, std::move(coef), -ref.sign});
                        }

                        // RHS becomes data-only: DECIDE vars replaced with constant 0
                        constraint->rhs_expr = StripDecideVars(*comp.right, op);
                    } else if (lhs->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                        // Simple single-variable constraint (e.g., x <= 5)
                        idx_t var_idx = op.FindDecideVariable(*lhs);
                        if (var_idx != DConstants::INVALID_INDEX) {
                            constraint->lhs_terms.push_back(Term{
                                var_idx,
                                make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))
                            });
                        }
                    } else {
                        // Multi-variable per-row constraint with complex LHS
                        // (e.g., z_0 + z_1 = 1, or x + (-3)*z_0 + (-5)*z_1 = 0,
                        //  or POWER(x - target, 2) <= K quadratic constraint)
                        ExtractConstraintTerms(*lhs, *constraint, 1);
                    }
                }

                constraints.push_back(std::move(constraint));
                break;
            }

            default:
                break;
        }
    }

    //! Walk a SUM argument expression tree and split into linear terms and bilinear terms.
    //! Bilinear terms (x * y where both are decide variables) go to objective->bilinear_terms.
    //! Linear terms (c * x, constants) go to objective->terms via ExtractTerms.
    void ExtractLinearAndBilinearTerms(const Expression &expr, Objective &obj, int sign,
                                       const Expression *filter = nullptr) {
        // PackDB: detect quadratic patterns (POWER / x*x / negated / const * POWER)
        // *before* any linear-structure traversal. This allows mixed shapes like
        // SUM(POWER(x-t, 2) + penalty*x) to route the POWER leaf into squared_terms
        // while the `+` recursion below sends the linear sibling into terms.
        //
        // The objective currently supports exactly one quadratic group per
        // objective (the inner expression of a single SUM(POWER(...))), with a
        // single scalar quadratic_sign. Additional quadratic groups (e.g.
        // `SUM(POWER(x,2)) + SUM(POWER(y,2))`) would need per-group Q matrices
        // downstream and are explicitly rejected.
        auto quad_pattern = op.DetectQuadraticPattern(expr);
        if (quad_pattern.inner_linear_expr) {
            double effective_sign = quad_pattern.sign * static_cast<double>(sign);
            if (obj.has_quadratic) {
                throw InvalidInputException(
                    "DECIDE objective contains multiple quadratic (POWER / (expr)*(expr)) "
                    "groups. Only a single quadratic group plus linear terms is supported; "
                    "combine them mathematically or rewrite the objective."
                );
            }
            obj.has_quadratic = true;
            obj.quadratic_sign = effective_sign;
            idx_t before = obj.squared_terms.size();
            op.ExtractTerms(*quad_pattern.inner_linear_expr, obj.squared_terms);
            if (filter) {
                for (idx_t i = before; i < obj.squared_terms.size(); i++) {
                    obj.squared_terms[i].filter = filter->Copy();
                }
            }
            return;
        }

        if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
            auto &func = expr.Cast<BoundFunctionExpression>();
            string fname = func.function.name;

            // Addition: recurse on all children
            if (fname == "+") {
                for (auto &child : func.children) {
                    ExtractLinearAndBilinearTerms(*child, obj, sign, filter);
                }
                return;
            }

            // Subtraction: first child same sign, second negated
            if (fname == "-" && func.children.size() == 2) {
                ExtractLinearAndBilinearTerms(*func.children[0], obj, sign, filter);
                ExtractLinearAndBilinearTerms(*func.children[1], obj, -sign, filter);
                return;
            }

            // Unary negation
            if (fname == "-" && func.children.size() == 1) {
                ExtractLinearAndBilinearTerms(*func.children[0], obj, -sign, filter);
                return;
            }

            // Multiplication: check for bilinear
            if (fname == "*" && func.children.size() == 2) {
                // Check if both sides contain decide variables
                idx_t left_var = op.FindDecideVariable(*func.children[0]);
                idx_t right_var = op.FindDecideVariable(*func.children[1]);

                if (left_var != DConstants::INVALID_INDEX && right_var != DConstants::INVALID_INDEX) {
                    // Both sides have decide variables
                    // Check for identical expression (QP, not bilinear)
                    if (func.children[0]->ToString() == func.children[1]->ToString()) {
                        // Fall through to linear extraction (shouldn't happen — QP detected earlier)
                    } else {
                        // Guard against degree > 2 shapes: each side must be linear
                        // in decide vars. Rejects e.g. x * POWER(y, 2) (degree 3),
                        // POWER(x, 2) * POWER(y, 2) (degree 4), (x*y) * z (degree 3).
                        // Without this gate the bilinear emitter silently treats the
                        // inner POWER / nested-* as an opaque "data coefficient",
                        // corrupting Q and in practice crashing the coefficient
                        // evaluator when the coefficient re-references a decide var.
                        if (!op.IsLinearInDecideVars(*func.children[0]) ||
                            !op.IsLinearInDecideVars(*func.children[1])) {
                            throw InvalidInputException(
                                "DECIDE objective contains a product of decision variables "
                                "with total degree > 2 (e.g. x * POWER(y, 2), POWER(x, 2) * "
                                "POWER(y, 2), x * x * y). Only bilinear (x * y, different "
                                "variables, each linear) or quadratic (POWER(linear_expr, 2)) "
                                "forms are supported.");
                        }
                        // Bilinear term: extract var_a, var_b, and optional data coefficient
                        // The expression is of the form: coef_a * var_a * coef_b * var_b
                        // We need to extract both variable indices and any remaining data coefficient.
                        // Simple case: left = var_a, right = var_b (coefficient = 1.0)
                        // Complex case: left = coef * var_a, right = var_b
                        // For now, handle the simple cases where each side is either:
                        //   - a bare variable reference, or
                        //   - data_coef * variable
                        idx_t var_a = left_var;
                        idx_t var_b = right_var;
                        unique_ptr<Expression> coef_a = op.ExtractCoefficientWithoutVariable(*func.children[0], var_a);
                        unique_ptr<Expression> coef_b = op.ExtractCoefficientWithoutVariable(*func.children[1], var_b);

                        // Multiply the two data coefficients together
                        unique_ptr<Expression> combined_coef;
                        bool a_is_one = (coef_a->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
                                         coef_a->Cast<BoundConstantExpression>().value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>() == 1.0);
                        bool b_is_one = (coef_b->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
                                         coef_b->Cast<BoundConstantExpression>().value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>() == 1.0);
                        if (a_is_one && b_is_one) {
                            combined_coef = nullptr; // coefficient is 1.0
                        } else if (a_is_one) {
                            combined_coef = std::move(coef_b);
                        } else if (b_is_one) {
                            combined_coef = std::move(coef_a);
                        } else {
                            // Need to create a multiplication expression
                            auto mul = make_uniq<BoundFunctionExpression>(
                                LogicalType::DOUBLE, func.function,
                                vector<unique_ptr<Expression>>(), nullptr);
                            // We need to build coef_a * coef_b — but we don't have the function binding.
                            // Simpler: just keep one of them as the coefficient and handle at eval time
                            // Actually, we can create the multiplication differently. For now,
                            // just store coef_a and handle coef_b multiplication in the coefficient.
                            // This is a simplification — a full solution would combine them.
                            combined_coef = std::move(coef_a);
                            // TODO: properly multiply coef_a * coef_b
                        }

                        Objective::BilinearTerm bt;
                        bt.var_a = var_a;
                        bt.var_b = var_b;
                        bt.coefficient = combined_coef ? std::move(combined_coef) : nullptr;
                        bt.sign = sign;
                        if (filter) {
                            bt.filter = filter->Copy();
                        }
                        obj.bilinear_terms.push_back(std::move(bt));
                        obj.has_bilinear = true;
                        return;
                    }
                }
            }
        }

        // Handle casts
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            auto &cast = expr.Cast<BoundCastExpression>();
            ExtractLinearAndBilinearTerms(*cast.child, obj, sign, filter);
            return;
        }

        // Not bilinear — delegate to linear extraction
        idx_t before = obj.terms.size();
        op.ExtractTerms(expr, obj.terms);
        // Apply sign to newly added terms
        if (sign == -1) {
            for (idx_t i = before; i < obj.terms.size(); i++) {
                obj.terms[i].sign *= -1;
            }
        }
        if (filter) {
            for (idx_t i = before; i < obj.terms.size(); i++) {
                obj.terms[i].filter = filter->Copy();
            }
        }
    }

    //! Extract linear and bilinear terms from a SUM argument in a constraint.
    //! Similar to ExtractLinearAndBilinearTerms but outputs to DecideConstraint fields.
    void ExtractConstraintTerms(const Expression &expr, DecideConstraint &constr, int sign,
                                const Expression *filter = nullptr) {
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
            auto &func = expr.Cast<BoundFunctionExpression>();
            string fname = func.function.name;

            if (fname == "+") {
                for (auto &child : func.children) {
                    ExtractConstraintTerms(*child, constr, sign, filter);
                }
                return;
            }
            if (fname == "-" && func.children.size() == 2) {
                ExtractConstraintTerms(*func.children[0], constr, sign, filter);
                ExtractConstraintTerms(*func.children[1], constr, -sign, filter);
                return;
            }
            if (fname == "-" && func.children.size() == 1) {
                ExtractConstraintTerms(*func.children[0], constr, -sign, filter);
                return;
            }
            // Helper: try to detect POWER(expr, 2), POW(expr, 2), expr ** 2,
            // or (expr)*(expr) self-product. Returns the inner expression on success.
            auto TryDetectConstraintQuadratic = [&](const Expression *test_expr) -> const Expression * {
                while (test_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    test_expr = test_expr->Cast<BoundCastExpression>().child.get();
                }
                if (test_expr->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) return nullptr;
                auto &qf = test_expr->Cast<BoundFunctionExpression>();
                string qname = StringUtil::Lower(qf.function.name);
                // POWER/POW/** with exponent 2
                if ((qname == "power" || qname == "pow" || qname == "**") && qf.children.size() == 2) {
                    const Expression *exp_expr = qf.children[1].get();
                    while (exp_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                        exp_expr = exp_expr->Cast<BoundCastExpression>().child.get();
                    }
                    if (exp_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                        double exponent = exp_expr->Cast<BoundConstantExpression>()
                                              .value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                        if (exponent == 2.0) {
                            const Expression *inner = qf.children[0].get();
                            while (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                                inner = inner->Cast<BoundCastExpression>().child.get();
                            }
                            if (op.FindDecideVariable(*inner) != DConstants::INVALID_INDEX) {
                                if (!op.IsLinearInDecideVars(*inner)) {
                                    throw InvalidInputException(
                                        "DECIDE constraint contains a non-linear expression "
                                        "inside POWER(..., 2) (total degree > 2 in decision "
                                        "variables). Only POWER(linear_expr, 2) is supported.");
                                }
                                return inner;
                            }
                        }
                    }
                }
                // Self-product: (expr)*(expr) with identical sides
                if (qname == "*" && qf.children.size() == 2 &&
                    qf.children[0]->ToString() == qf.children[1]->ToString() &&
                    op.FindDecideVariable(*qf.children[0]) != DConstants::INVALID_INDEX) {
                    const Expression *inner = qf.children[0].get();
                    while (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                        inner = inner->Cast<BoundCastExpression>().child.get();
                    }
                    if (!op.IsLinearInDecideVars(*inner)) {
                        throw InvalidInputException(
                            "DECIDE constraint contains a self-product of a non-linear "
                            "expression (e.g. POWER(x, 2) * POWER(x, 2)), total degree > 2 "
                            "in decision variables. Only (linear_expr) * (linear_expr) is "
                            "supported as a quadratic pattern.");
                    }
                    return inner;
                }
                return nullptr;
            };

            // Direct POWER/self-product detection
            {
                const Expression *inner = TryDetectConstraintQuadratic(&func);
                if (inner) {
                    DecideConstraint::QuadraticGroup qg;
                    qg.sign = static_cast<double>(sign);
                    if (filter) {
                        qg.filter = filter->Copy();
                    }
                    op.ExtractTerms(*inner, qg.inner_terms);
                    constr.quadratic_groups.push_back(std::move(qg));
                    constr.has_quadratic = true;
                    return;
                }
            }
            if (fname == "*" && func.children.size() == 2) {
                // Scaled quadratic: const * POWER(expr, 2) or POWER(expr, 2) * const
                for (idx_t side = 0; side < 2; side++) {
                    const Expression *maybe_const = func.children[side].get();
                    while (maybe_const->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                        maybe_const = maybe_const->Cast<BoundCastExpression>().child.get();
                    }
                    if (maybe_const->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                        double cval = maybe_const->Cast<BoundConstantExpression>()
                                          .value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                        if (cval != 0.0) {
                            const Expression *inner = TryDetectConstraintQuadratic(func.children[1 - side].get());
                            if (inner) {
                                DecideConstraint::QuadraticGroup qg;
                                qg.sign = static_cast<double>(sign) * cval;
                                if (filter) {
                                    qg.filter = filter->Copy();
                                }
                                op.ExtractTerms(*inner, qg.inner_terms);
                                constr.quadratic_groups.push_back(std::move(qg));
                                constr.has_quadratic = true;
                                return;
                            }
                        }
                    }
                }
                // Self-product (expr)*(expr): handled by TryDetectConstraintQuadratic above
                // Bilinear constraint term: var_a * var_b with different variables
                idx_t left_var = op.FindDecideVariable(*func.children[0]);
                idx_t right_var = op.FindDecideVariable(*func.children[1]);
                if (left_var != DConstants::INVALID_INDEX && right_var != DConstants::INVALID_INDEX &&
                    func.children[0]->ToString() != func.children[1]->ToString()) {
                    // Guard against degree > 2 shapes in constraints (mirrors the
                    // objective-side check). Without this, x * POWER(y,2) silently
                    // becomes a bilinear x*y with POWER(y,2) carried as an opaque
                    // "coefficient" that still references y.
                    if (!op.IsLinearInDecideVars(*func.children[0]) ||
                        !op.IsLinearInDecideVars(*func.children[1])) {
                        throw InvalidInputException(
                            "DECIDE constraint contains a product of decision variables "
                            "with total degree > 2 (e.g. x * POWER(y, 2), POWER(x, 2) * "
                            "POWER(y, 2), x * x * y). Only bilinear (x * y, different "
                            "variables, each linear) or quadratic (POWER(linear_expr, 2)) "
                            "forms are supported.");
                    }
                    BilinearConstraintTerm bt;
                    bt.var_a = left_var;
                    bt.var_b = right_var;
                    auto coef_a = op.ExtractCoefficientWithoutVariable(*func.children[0], left_var);
                    auto coef_b = op.ExtractCoefficientWithoutVariable(*func.children[1], right_var);
                    bool a_one = (coef_a->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
                                  coef_a->Cast<BoundConstantExpression>().value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>() == 1.0);
                    bool b_one = (coef_b->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
                                  coef_b->Cast<BoundConstantExpression>().value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>() == 1.0);
                    if (!a_one) {
                        bt.coefficient = std::move(coef_a);
                    } else if (!b_one) {
                        bt.coefficient = std::move(coef_b);
                    }
                    bt.sign = sign;
                    if (filter) {
                        bt.filter = filter->Copy();
                    }
                    constr.bilinear_terms.push_back(std::move(bt));
                    constr.has_bilinear = true;
                    return;
                }
            }
        }
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            auto &cast = expr.Cast<BoundCastExpression>();
            ExtractConstraintTerms(*cast.child, constr, sign, filter);
            return;
        }
        // Linear — delegate to ExtractTerms
        idx_t before = constr.lhs_terms.size();
        op.ExtractTerms(expr, constr.lhs_terms);
        if (sign == -1) {
            for (idx_t i = before; i < constr.lhs_terms.size(); i++) {
                constr.lhs_terms[i].sign *= -1;
            }
        }
        if (filter) {
            for (idx_t i = before; i < constr.lhs_terms.size(); i++) {
                constr.lhs_terms[i].filter = filter->Copy();
            }
        }
    }

    void ExtractAggregateObjectiveTerms(const Expression &expr, Objective &obj, int sign) {
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            auto &cast = expr.Cast<BoundCastExpression>();
            ExtractAggregateObjectiveTerms(*cast.child, obj, sign);
            return;
        }
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
            auto &func = expr.Cast<BoundFunctionExpression>();
            if (func.function.name == "+") {
                for (auto &child : func.children) {
                    ExtractAggregateObjectiveTerms(*child, obj, sign);
                }
                return;
            }
            if (func.function.name == "-" && func.children.size() == 2) {
                ExtractAggregateObjectiveTerms(*func.children[0], obj, sign);
                ExtractAggregateObjectiveTerms(*func.children[1], obj, -sign);
                return;
            }
            if (func.function.name == "-" && func.children.size() == 1) {
                ExtractAggregateObjectiveTerms(*func.children[0], obj, -sign);
                return;
            }
        }
        if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
            throw InternalException("DECIDE objective contains a non-aggregate term: %s", expr.ToString());
        }

        auto &agg = expr.Cast<BoundAggregateExpression>();
        auto agg_name = StringUtil::Lower(agg.function.name);
        if (agg_name != "sum") {
            throw InternalException("DECIDE optimizer should rewrite objective aggregate '%s' to SUM before execution",
                                    agg.function.name);
        }
        bool is_avg = (agg.alias == AVG_REWRITE_TAG);

        idx_t before = obj.terms.size();
        idx_t bilinear_before = obj.bilinear_terms.size();
        idx_t squared_before = obj.squared_terms.size();
        ExtractLinearAndBilinearTerms(*agg.children[0], obj, sign, agg.filter.get());
        for (idx_t i = before; i < obj.terms.size(); i++) {
            obj.terms[i].avg_scale = is_avg;
        }
        for (idx_t i = bilinear_before; i < obj.bilinear_terms.size(); i++) {
            obj.bilinear_terms[i].avg_scale = is_avg;
        }
        for (idx_t i = squared_before; i < obj.squared_terms.size(); i++) {
            obj.squared_terms[i].avg_scale = is_avg;
        }
    }

    void AnalyzeObjective(const unique_ptr<Expression>& expr_ptr) {
        auto *expr = expr_ptr.get();
        while (expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            expr = expr->Cast<BoundCastExpression>().child.get();
        }

        // PackDB: Check for PER [STRICT] wrapper on objective (outermost layer)
        vector<unique_ptr<Expression>> per_cols;
        bool obj_per_strict = false;
        if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
            auto &conj = expr->Cast<BoundConjunctionExpression>();
            if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
                obj_per_strict = IsPerStrictTag(conj.alias);
                for (idx_t i = 1; i < conj.children.size(); i++) {
                    per_cols.push_back(conj.children[i]->Copy());
                }
                expr = conj.children[0].get();
                while (expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    expr = expr->Cast<BoundCastExpression>().child.get();
                }
            }
        }

        // PackDB: Check for WHEN wrapper on objective (inside PER, if present)
        unique_ptr<Expression> when_cond;
        if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
            auto &conj = expr->Cast<BoundConjunctionExpression>();
            if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
                when_cond = conj.children[1]->Copy();
                // Unwrap to get the actual objective expression
                expr = conj.children[0].get();
                while (expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    expr = expr->Cast<BoundCastExpression>().child.get();
                }
            }
        }

        if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
            auto &agg = expr->Cast<BoundAggregateExpression>();

            objective = make_uniq<Objective>();

            // Walk the SUM argument. ExtractLinearAndBilinearTerms recognises
            // quadratic patterns (POWER/(expr)*(expr)/negated/K*POWER) at any
            // position in `+`/`-` trees and routes them into squared_terms, so
            // the same walker handles pure QP, pure linear+bilinear, and the
            // mixed forms (e.g. SUM(POWER(x-t, 2) + penalty*x)) uniformly.
            idx_t before = objective->terms.size();
            idx_t bilinear_before = objective->bilinear_terms.size();
            idx_t squared_before = objective->squared_terms.size();
            ExtractLinearAndBilinearTerms(*agg.children[0], *objective, 1, agg.filter.get());
            if (agg.alias == AVG_REWRITE_TAG) {
                for (idx_t i = before; i < objective->terms.size(); i++) {
                    objective->terms[i].avg_scale = true;
                }
                for (idx_t i = bilinear_before; i < objective->bilinear_terms.size(); i++) {
                    objective->bilinear_terms[i].avg_scale = true;
                }
                for (idx_t i = squared_before; i < objective->squared_terms.size(); i++) {
                    objective->squared_terms[i].avg_scale = true;
                }
            }

            objective->when_condition = std::move(when_cond);
            objective->per_columns = std::move(per_cols);
            objective->per_strict = obj_per_strict;
        } else if (BoundExpressionContainsAggregate(*expr)) {
            objective = make_uniq<Objective>();
            ExtractAggregateObjectiveTerms(*expr, *objective, 1);
            objective->when_condition = std::move(when_cond);
            objective->per_columns = std::move(per_cols);
            objective->per_strict = obj_per_strict;
        }
    }

    //===--------------------------------------------------------------------===//
    // Variable Bounds Extraction (Part 3)
    //===--------------------------------------------------------------------===//

    void ExtractVariableBounds(vector<double> &lower_bounds, vector<double> &upper_bounds) {
        // Traverse decide_constraints to find variable-level bounds
        TraverseBoundsConstraints(*op.decide_constraints, lower_bounds, upper_bounds);
    }

    void TraverseBoundsConstraints(const Expression &expr,
                                   vector<double> &lower_bounds,
                                   vector<double> &upper_bounds) {
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                // PackDB PER: only recurse into the constraint (child[0]), skip the columns
                if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
                    TraverseBoundsConstraints(*conj.children[0], lower_bounds, upper_bounds);
                    break;
                }
                // PackDB WHEN: skip entirely — WHEN constraints are conditional (per-row),
                // so they must NOT contribute to global variable bounds.
                // E.g., "x <= 0 WHEN condition" should NOT set upper_bounds[x] = 0 globally.
                if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
                    break;
                }
                // AND expression - recurse on all children
                for (auto &child : conj.children) {
                    TraverseBoundsConstraints(*child, lower_bounds, upper_bounds);
                }
                break;
            }

            case ExpressionClass::BOUND_COMPARISON: {
                auto &comp = expr.Cast<BoundComparisonExpression>();

                // Only extract global bounds from simple "x OP constant" constraints,
                // where x is a bare DECIDE variable (possibly cast-wrapped).
                // Multi-variable expressions (e.g., x - 3*z_1 - 5*z_2 = 0 from IN rewrite)
                // must NOT be treated as single-variable bounds.
                auto *lhs = comp.left.get();
                while (lhs->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    lhs = lhs->Cast<BoundCastExpression>().child.get();
                }

                if (lhs->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    // Simple single-variable LHS — check if it's a DECIDE variable
                    auto &colref = lhs->Cast<BoundColumnRefExpression>();
                    idx_t var_idx = DConstants::INVALID_INDEX;
                    for (idx_t i = 0; i < op.decide_variables.size(); i++) {
                        auto &decide_var = op.decide_variables[i]->Cast<BoundColumnRefExpression>();
                        if (colref.binding == decide_var.binding) {
                            var_idx = i;
                            break;
                        }
                    }

                    if (var_idx != DConstants::INVALID_INDEX) {
                        // Extract bound value from RHS, unwrapping CASTs
                        // (DuckDB may insert implicit casts like CAST(5 AS INTEGER))
                        auto *rhs_ptr = comp.right.get();
                        while (rhs_ptr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                            rhs_ptr = rhs_ptr->Cast<BoundCastExpression>().child.get();
                        }
                        if (rhs_ptr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                            auto &rhs = rhs_ptr->Cast<BoundConstantExpression>();

                            // Cast to double - handle both INTEGER and DOUBLE types
                            double bound_value;
                            if (rhs.value.type().id() == LogicalTypeId::INTEGER ||
                                rhs.value.type().id() == LogicalTypeId::BIGINT) {
                                bound_value = static_cast<double>(rhs.value.GetValue<int64_t>());
                            } else if (rhs.value.type().id() == LogicalTypeId::DOUBLE ||
                                       rhs.value.type().id() == LogicalTypeId::FLOAT) {
                                bound_value = rhs.value.GetValue<double>();
                            } else {
                                // Try default cast
                                bound_value = rhs.value.GetValue<double>();
                            }

                            // Apply bound based on comparison type
                            if (comp.type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
                                // x <= bound
                                upper_bounds[var_idx] = std::min(upper_bounds[var_idx], bound_value);
                            } else if (comp.type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
                                // x >= bound
                                lower_bounds[var_idx] = std::max(lower_bounds[var_idx], bound_value);
                            } else if (comp.type == ExpressionType::COMPARE_EQUAL) {
                                // x = bound (if enabled in future)
                                lower_bounds[var_idx] = bound_value;
                                upper_bounds[var_idx] = bound_value;
                            }
                        }
                    }
                }
                break;
            }

            case ExpressionClass::BOUND_CONSTANT: {
                // Type declarations return dummy constants - skip them
                break;
            }

            default:
                break;
        }
    }

    mutex lock;
    // This collection will hold all the data from the child operator
    ColumnDataCollection data;

    const PhysicalDecide &op;

    vector<unique_ptr<DecideConstraint>> constraints;
    unique_ptr<Objective> objective;

    //===--------------------------------------------------------------------===//
    // Evaluated Coefficients (Phase 2)
    //===--------------------------------------------------------------------===//

    vector<EvaluatedConstraint> evaluated_constraints;
    vector<vector<double>> evaluated_objective_coefficients;  // [term_idx][row_idx]
    vector<idx_t> objective_variable_indices;

    // Quadratic objective: evaluated inner linear expression coefficients
    vector<vector<double>> evaluated_quadratic_coefficients;  // [term_idx][row_idx]
    vector<idx_t> quadratic_variable_indices;
    bool has_quadratic_objective = false;
    double quadratic_sign = 1.0;

    // Bilinear objective: pairs of different decide variables with data coefficients
    struct EvaluatedBilinearTerm {
        idx_t var_a;
        idx_t var_b;
        vector<double> row_coefficients; // [row_idx]
    };
    vector<EvaluatedBilinearTerm> evaluated_bilinear_terms;

    vector<double> ilp_solution;
    VarIndexer var_indexer;  // For mapping (var_idx, row) to solution indices
};

class DecideLocalSinkState : public LocalSinkState {
public:
    explicit DecideLocalSinkState(ClientContext &context, const PhysicalDecide &op)
        : data(context, op.children[0]->GetTypes()) {
        data.InitializeAppend(append_state);
    }

    // A local collection to buffer chunks before merging into the global state
    ColumnDataCollection data;
    ColumnDataAppendState append_state;
};

unique_ptr<GlobalSinkState> PhysicalDecide::GetGlobalSinkState(ClientContext &context) const {
    return make_uniq_base<GlobalSinkState, DecideGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalDecide::GetLocalSinkState(ExecutionContext &context) const {
    return make_uniq_base<LocalSinkState, DecideLocalSinkState>(context.client, *this);
}

SinkResultType PhysicalDecide::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
    auto &lstate = input.local_state.Cast<DecideLocalSinkState>();
    lstate.data.Append(lstate.append_state, chunk);
    return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalDecide::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    auto &lstate = input.local_state.Cast<DecideLocalSinkState>();

    lock_guard<mutex> guard(gstate.lock);
    gstate.data.Combine(lstate.data);

    return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalDecide::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
    bool bench = std::getenv("PACKDB_BENCH") != nullptr;
    Profiler model_timer;
    Profiler solver_timer;

    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    idx_t num_rows = gstate.data.Count();

    if (bench) {
        model_timer.Start();
    }

    // Empty input → return empty result (mirrors standard SQL behavior)
    if (num_rows == 0) {
        return SinkFinalizeType::READY;
    }

    idx_t num_decide_vars = decide_variables.size();
    if (num_decide_vars == 0) {
        throw InternalException(
            "DECIDE operator has no decision variables "
            "(should have been caught during binding)");
    }

    // Evaluate coefficients and build the model (solver provides verbose output)

    //===--------------------------------------------------------------------===//
    // PHASE 1.5: Build Entity Mappings for Table-Scoped Variables
    //===--------------------------------------------------------------------===//

    vector<EntityMapping> entity_mappings;
    for (idx_t scope_idx = 0; scope_idx < entity_scopes.size(); scope_idx++) {
        auto &scope = entity_scopes[scope_idx];
        EntityMapping mapping;
        mapping.row_to_entity.resize(num_rows);

        // Read entity key column values directly from the data chunk
        // using the pre-resolved physical column indices
        idx_t num_key_cols = scope.entity_key_physical_indices.size();
        vector<vector<Value>> key_columns(num_key_cols);

        for (idx_t col_idx = 0; col_idx < num_key_cols; col_idx++) {
            key_columns[col_idx].reserve(num_rows);
        }

        {
            ColumnDataScanState key_scan_state;
            gstate.data.InitializeScan(key_scan_state);
            DataChunk key_chunk;
            key_chunk.Initialize(context, gstate.data.Types());

            while (gstate.data.Scan(key_scan_state, key_chunk)) {
                for (idx_t row_in_chunk = 0; row_in_chunk < key_chunk.size(); row_in_chunk++) {
                    for (idx_t col_idx = 0; col_idx < num_key_cols; col_idx++) {
                        idx_t phys_idx = scope.entity_key_physical_indices[col_idx];
                        key_columns[col_idx].push_back(key_chunk.data[phys_idx].GetValue(row_in_chunk));
                    }
                }
            }
        }

        // Build composite key → entity_id map (same approach as PER grouping)
        unordered_map<string, idx_t> key_to_entity;
        idx_t next_entity = 0;

        for (idx_t row = 0; row < num_rows; row++) {
            string key;
            for (idx_t col_idx = 0; col_idx < num_key_cols; col_idx++) {
                if (col_idx > 0) {
                    key.push_back('\0');
                }
                // Prefix each value with a NULL/non-NULL tag to avoid collisions
                // between SQL NULL and the literal string "NULL"
                auto &val = key_columns[col_idx][row];
                if (val.IsNull()) {
                    key.push_back('\x00');
                } else {
                    key.push_back('\x01');
                    key += val.ToString();
                }
            }
            auto it = key_to_entity.find(key);
            if (it == key_to_entity.end()) {
                key_to_entity[key] = next_entity;
                mapping.row_to_entity[row] = next_entity;
                next_entity++;
            } else {
                mapping.row_to_entity[row] = it->second;
            }
        }
        mapping.num_entities = next_entity;
        entity_mappings.push_back(std::move(mapping));
    }

    //===--------------------------------------------------------------------===//
    // PHASE 2: Evaluate Coefficient Expressions
    //===--------------------------------------------------------------------===//

    //! Per-term filter state for aggregate-local WHEN.
    struct TermFilterState {
        vector<bool> mask;
        bool has_filter = false;
        bool avg_scale = false;
    };

    auto EvaluateBooleanMask = [&](const Expression &condition) {
        vector<bool> mask;
        auto transformed_condition = TransformToChunkExpression(condition, context);
        ExpressionExecutor cond_executor(context);
        cond_executor.AddExpression(*transformed_condition);

        mask.reserve(num_rows);
        ColumnDataScanState cond_scan_state;
        gstate.data.InitializeScan(cond_scan_state);
        DataChunk cond_chunk;
        cond_chunk.Initialize(context, gstate.data.Types());

        while (gstate.data.Scan(cond_scan_state, cond_chunk)) {
            DataChunk cond_result;
            cond_result.Initialize(context, {LogicalType::BOOLEAN});
            cond_executor.Execute(cond_chunk, cond_result);

            auto &vec = cond_result.data[0];
            for (idx_t row_in_chunk = 0; row_in_chunk < cond_chunk.size(); row_in_chunk++) {
                Value val = vec.GetValue(row_in_chunk);
                mask.push_back(val.IsNull() ? false : val.GetValue<bool>());
            }
        }
        return mask;
    };

    // 1. Evaluate constraints
    for (idx_t c = 0; c < gstate.constraints.size(); c++) {
        auto &constraint = gstate.constraints[c];

        EvaluatedConstraint eval_const;
        eval_const.comparison_type = constraint->comparison_type;
        // Preserve whether the original LHS was an aggregate (e.g., SUM(...))
        eval_const.lhs_is_aggregate = constraint->lhs_is_aggregate;
        eval_const.minmax_indicator_idx = constraint->minmax_indicator_idx;
        eval_const.minmax_agg_type = constraint->minmax_agg_type;
        eval_const.ne_indicator_idx = constraint->ne_indicator_idx;
        eval_const.per_strict = constraint->per_strict;

        // Initialize result storage
        eval_const.row_coefficients.resize(constraint->lhs_terms.size());

        // Scan data and evaluate LHS coefficients
        ColumnDataScanState scan_state;
        gstate.data.InitializeScan(scan_state);

        DataChunk chunk;
        chunk.Initialize(context, gstate.data.Types());

        // Store variable indices for all terms (before scanning data)
        for (auto &term : constraint->lhs_terms) {
            eval_const.variable_indices.push_back(term.variable_index);
        }

        vector<TermFilterState> term_filters(constraint->lhs_terms.size());
        vector<TermFilterState> bilinear_filters(constraint->bilinear_terms.size());
        vector<TermFilterState> quadratic_filters(constraint->quadratic_groups.size());
        vector<bool> local_row_active(num_rows, false);
        bool has_local_filters = false;
        bool has_unfiltered_aggregate_part = false;

        auto RegisterLocalFilter = [&](const unique_ptr<Expression> &filter, TermFilterState &state) {
            state.mask = EvaluateBooleanMask(*filter);
            if (state.mask.size() != num_rows) {
                throw InternalException("DECIDE aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                                        num_rows, state.mask.size());
            }
            state.has_filter = true;
            has_local_filters = true;
            for (idx_t row = 0; row < num_rows; row++) {
                if (state.mask[row]) {
                    local_row_active[row] = true;
                }
            }
        };

        if (constraint->lhs_is_aggregate) {
            for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
                auto &term = constraint->lhs_terms[term_idx];
                term_filters[term_idx].avg_scale = term.avg_scale;
                if (term.filter) {
                    RegisterLocalFilter(term.filter, term_filters[term_idx]);
                } else {
                    has_unfiltered_aggregate_part = true;
                }
            }
            for (idx_t term_idx = 0; term_idx < constraint->bilinear_terms.size(); term_idx++) {
                auto &term = constraint->bilinear_terms[term_idx];
                bilinear_filters[term_idx].avg_scale = term.avg_scale;
                if (term.filter) {
                    RegisterLocalFilter(term.filter, bilinear_filters[term_idx]);
                } else {
                    has_unfiltered_aggregate_part = true;
                }
            }
            for (idx_t group_idx = 0; group_idx < constraint->quadratic_groups.size(); group_idx++) {
                auto &group = constraint->quadratic_groups[group_idx];
                quadratic_filters[group_idx].avg_scale = group.avg_scale;
                if (group.filter) {
                    RegisterLocalFilter(group.filter, quadratic_filters[group_idx]);
                } else {
                    has_unfiltered_aggregate_part = true;
                }
            }
            if (has_unfiltered_aggregate_part) {
                std::fill(local_row_active.begin(), local_row_active.end(), true);
            }
        }

        while (gstate.data.Scan(scan_state, chunk)) {
            // Evaluate each term separately for this chunk
            for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
                auto &term = constraint->lhs_terms[term_idx];

                auto transformed_coef = TransformToChunkExpression(*term.coefficient, context);

                // Create executor and evaluate this term
                ExpressionExecutor term_executor(context);
                try {
                    term_executor.AddExpression(*transformed_coef);
                } catch (const std::exception &e) {
                    throw InternalException("Failed to add expression for term %llu: %s\nOriginal: %s\nTransformed: %s",
                        term_idx, e.what(), term.coefficient->ToString(), transformed_coef->ToString());
                }

                // Execute on chunk
                // Use the expression's actual return type, then cast to double when extracting
                DataChunk term_result;
                vector<LogicalType> result_types = {transformed_coef->return_type};
                term_result.Initialize(context, result_types);
                term_executor.Execute(chunk, term_result);

                // Extract values and cast to double
                auto &vec = term_result.data[0];
                for (idx_t row_in_chunk = 0; row_in_chunk < chunk.size(); row_in_chunk++) {
                    // Cast to double regardless of the actual type (could be INTEGER, DOUBLE, etc.)
                    Value val = vec.GetValue(row_in_chunk);

                    // Check for NULL values
                    if (val.IsNull()) {
                        throw InvalidInputException(
                            "DECIDE constraint coefficient returned NULL at row %llu. "
                            "NULL values are not allowed in optimization coefficients. "
                            "Use COALESCE() to handle NULLs or filter them with WHERE clause.",
                            eval_const.row_coefficients[term_idx].size());
                    }

                    double double_val = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();

                    // Check for NaN or Infinity
                    if (!std::isfinite(double_val)) {
                        throw InvalidInputException(
                            "DECIDE constraint coefficient contains invalid value (NaN or Infinity) at row %llu. "
                            "Common causes:\n"
                            "  • Division by zero in coefficient expression\n"
                            "  • Arithmetic overflow in calculations\n"
                            "  • NULL values that propagated through math operations\n"
                            "Check your coefficient expressions and input data.",
                            eval_const.row_coefficients[term_idx].size());
                    }

                    eval_const.row_coefficients[term_idx].push_back(double_val * term.sign);
                }
            }
        }

        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            if (!term_filters[term_idx].has_filter) {
                continue;
            }
            auto &coefficients = eval_const.row_coefficients[term_idx];
            auto &mask = term_filters[term_idx].mask;
            for (idx_t row = 0; row < coefficients.size(); row++) {
                if (!mask[row]) {
                    coefficients[row] = 0.0;
                }
            }
        }

        // Evaluate RHS
        // RHS can be a constant, an aggregate (scalar), or a row-varying expression (for row-wise constraints)
        
        // Initialize RHS values vector
        eval_const.rhs_values.reserve(num_rows);

        if (constraint->rhs_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
            auto &const_expr = constraint->rhs_expr->Cast<BoundConstantExpression>();
            double rhs_constant = const_expr.value.GetValue<double>();
            eval_const.rhs_values.assign(num_rows, rhs_constant);
        } else {
            // RHS is a complex expression. It might be row-varying (e.g., column ref) or scalar (aggregate).
            // We evaluate it against the data chunks.
            
            auto transformed_rhs = TransformToChunkExpression(*constraint->rhs_expr, context, num_rows);

            // Prepare executor
            ExpressionExecutor rhs_executor(context);
            rhs_executor.AddExpression(*transformed_rhs);

            // Scan data and evaluate
            ColumnDataScanState rhs_scan_state;
            gstate.data.InitializeScan(rhs_scan_state);
            DataChunk rhs_chunk;
            rhs_chunk.Initialize(context, gstate.data.Types());

            while (gstate.data.Scan(rhs_scan_state, rhs_chunk)) {
                DataChunk rhs_result;
                vector<LogicalType> result_types = {transformed_rhs->return_type};
                rhs_result.Initialize(context, result_types);
                rhs_executor.Execute(rhs_chunk, rhs_result);

                auto &vec = rhs_result.data[0];
                for (idx_t row_in_chunk = 0; row_in_chunk < rhs_chunk.size(); row_in_chunk++) {
                    Value val = vec.GetValue(row_in_chunk);

                    // Check for NULL values
                    if (val.IsNull()) {
                        throw InvalidInputException(
                            "DECIDE constraint right-hand side returned NULL at row %llu. "
                            "NULL values are not allowed in optimization constraints. "
                            "Use COALESCE() to handle NULLs or filter them with WHERE clause.",
                            eval_const.rhs_values.size());
                    }

                    double double_val = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();

                    // Check for NaN or Infinity
                    if (!std::isfinite(double_val)) {
                        throw InvalidInputException(
                            "DECIDE constraint right-hand side contains invalid value (NaN or Infinity) at row %llu. "
                            "Common causes:\n"
                            "  • Division by zero in RHS expression\n"
                            "  • Arithmetic overflow in calculations\n"
                            "  • NULL values that propagated through math operations\n"
                            "Check your RHS expressions and input data.",
                            eval_const.rhs_values.size());
                    }

                    eval_const.rhs_values.push_back(double_val);
                }
            }
        }

        // PackDB: Unified WHEN+PER row→group assignment
        // Produces row_group_ids and num_groups for the evaluated constraint.
        // - No WHEN, no PER: row_group_ids stays empty, num_groups = 0 (fast path)
        // - WHEN only: row_group_ids[row] = 0 (matching) or INVALID_INDEX (excluded), num_groups = 1
        // - PER only: row_group_ids[row] = 0..K-1 (group id), INVALID_INDEX for NULL PER values, num_groups = K
        // - WHEN+PER: WHEN filters first, then PER groups the remaining rows
        bool has_when = (constraint->when_condition != nullptr);
        bool has_per = (!constraint->per_columns.empty());

        if (has_when || has_per || has_local_filters) {
            // Step 1: Evaluate WHEN condition (if present) to get per-row booleans
            vector<bool> when_mask;
            if (has_when) {
                when_mask = EvaluateBooleanMask(*constraint->when_condition);
            }

            // Step 2: Evaluate PER columns (if present) to get per-row values
            // For multi-column PER, we evaluate each column separately and build composite keys
            vector<vector<Value>> all_per_values;  // [col_idx][row_idx]
            if (has_per) {
                all_per_values.resize(constraint->per_columns.size());
                for (idx_t col_idx = 0; col_idx < constraint->per_columns.size(); col_idx++) {
                    auto transformed_col = TransformToChunkExpression(*constraint->per_columns[col_idx], context);
                    ExpressionExecutor per_executor(context);
                    per_executor.AddExpression(*transformed_col);

                    all_per_values[col_idx].reserve(num_rows);

                    ColumnDataScanState per_scan_state;
                    gstate.data.InitializeScan(per_scan_state);
                    DataChunk per_chunk;
                    per_chunk.Initialize(context, gstate.data.Types());

                    while (gstate.data.Scan(per_scan_state, per_chunk)) {
                        DataChunk per_result;
                        per_result.Initialize(context, {transformed_col->return_type});
                        per_executor.Execute(per_chunk, per_result);

                        auto &vec = per_result.data[0];
                        for (idx_t row_in_chunk = 0; row_in_chunk < per_chunk.size(); row_in_chunk++) {
                            all_per_values[col_idx].push_back(vec.GetValue(row_in_chunk));
                        }
                    }
                }
            }

            // Step 3: Build unified row_group_ids
            eval_const.row_group_ids.resize(num_rows);

            auto row_is_included = [&](idx_t row) {
                if (has_when && !when_mask[row]) {
                    return false;
                }
                if (has_local_filters && !local_row_active[row]) {
                    return false;
                }
                return true;
            };

            // Helper: build composite PER key for a row
            auto build_per_key = [&](idx_t row, bool &has_null_out) -> string {
                has_null_out = false;
                for (idx_t col_idx = 0; col_idx < all_per_values.size(); col_idx++) {
                    if (all_per_values[col_idx][row].IsNull()) {
                        has_null_out = true;
                        return {};
                    }
                }
                string key;
                for (idx_t col_idx = 0; col_idx < all_per_values.size(); col_idx++) {
                    if (col_idx > 0) {
                        key.push_back('\0');
                    }
                    key += all_per_values[col_idx][row].ToString();
                }
                return key;
            };

            if (has_per) {
                // PER (with or without WHEN)
                // Map distinct composite PER keys to group IDs (first-seen order)
                bool is_strict = constraint->per_strict;
                unordered_map<string, idx_t> value_to_group;
                idx_t next_group = 0;

                if (is_strict) {
                    // PER STRICT: Phase 1 — discover groups from ALL rows (ignoring WHEN)
                    for (idx_t row = 0; row < num_rows; row++) {
                        bool has_null = false;
                        string key = build_per_key(row, has_null);
                        if (has_null) continue;
                        if (value_to_group.find(key) == value_to_group.end()) {
                            value_to_group[key] = next_group++;
                        }
                    }
                    // Phase 2: Assign row_group_ids (WHEN-excluded → INVALID_INDEX)
                    for (idx_t row = 0; row < num_rows; row++) {
                        bool has_null = false;
                        string key = build_per_key(row, has_null);
                        if (has_null) {
                            eval_const.row_group_ids[row] = DConstants::INVALID_INDEX;
                            continue;
                        }
                        if (!row_is_included(row)) {
                            eval_const.row_group_ids[row] = DConstants::INVALID_INDEX;
                            continue;
                        }
                        eval_const.row_group_ids[row] = value_to_group[key];
                    }
                } else {
                    // Standard WHEN→PER: discover groups only from WHEN-qualifying rows
                    for (idx_t row = 0; row < num_rows; row++) {
                        if (!row_is_included(row)) {
                            eval_const.row_group_ids[row] = DConstants::INVALID_INDEX;
                            continue;
                        }
                        bool has_null = false;
                        string key = build_per_key(row, has_null);
                        if (has_null) {
                            eval_const.row_group_ids[row] = DConstants::INVALID_INDEX;
                            continue;
                        }
                        auto it = value_to_group.find(key);
                        if (it == value_to_group.end()) {
                            value_to_group[key] = next_group;
                            eval_const.row_group_ids[row] = next_group;
                            next_group++;
                        } else {
                            eval_const.row_group_ids[row] = it->second;
                        }
                    }
                }
                eval_const.num_groups = next_group;
            } else {
                // WHEN and/or aggregate-local WHEN (no PER): one group (group 0) for matching rows
                for (idx_t row = 0; row < num_rows; row++) {
                    eval_const.row_group_ids[row] = row_is_included(row) ? 0 : DConstants::INVALID_INDEX;
                }
                eval_const.num_groups = 1;
            }
        }

        auto ScaleAvgRowCoefficients = [&](vector<double> &coefficients, bool has_filter,
                                           const vector<bool> &filter_mask) {
            if (eval_const.row_group_ids.empty()) {
                idx_t denominator = 0;
                for (idx_t row = 0; row < num_rows; row++) {
                    if (!has_filter || filter_mask[row]) {
                        denominator++;
                    }
                }
                if (denominator == 0) {
                    std::fill(coefficients.begin(), coefficients.end(), 0.0);
                    return;
                }
                double scale = 1.0 / static_cast<double>(denominator);
                for (auto &coefficient : coefficients) {
                    coefficient *= scale;
                }
                return;
            }

            vector<idx_t> group_counts(eval_const.num_groups, 0);
            for (idx_t row = 0; row < num_rows; row++) {
                idx_t gid = eval_const.row_group_ids[row];
                if (gid == DConstants::INVALID_INDEX) {
                    continue;
                }
                if (!has_filter || filter_mask[row]) {
                    group_counts[gid]++;
                }
            }
            for (idx_t row = 0; row < coefficients.size(); row++) {
                idx_t gid = eval_const.row_group_ids[row];
                if (gid == DConstants::INVALID_INDEX || group_counts[gid] == 0) {
                    coefficients[row] = 0.0;
                    continue;
                }
                coefficients[row] /= static_cast<double>(group_counts[gid]);
            }
        };

        auto ScaleAvgQuadraticCoefficients = [&](vector<vector<double>> &row_coefficients, bool has_filter,
                                                 const vector<bool> &filter_mask) {
            if (eval_const.row_group_ids.empty()) {
                idx_t denominator = 0;
                for (idx_t row = 0; row < num_rows; row++) {
                    if (!has_filter || filter_mask[row]) {
                        denominator++;
                    }
                }
                if (denominator == 0) {
                    for (auto &coefficients : row_coefficients) {
                        std::fill(coefficients.begin(), coefficients.end(), 0.0);
                    }
                    return;
                }
                double scale = 1.0 / std::sqrt(static_cast<double>(denominator));
                for (auto &coefficients : row_coefficients) {
                    for (auto &coefficient : coefficients) {
                        coefficient *= scale;
                    }
                }
                return;
            }

            vector<idx_t> group_counts(eval_const.num_groups, 0);
            for (idx_t row = 0; row < num_rows; row++) {
                idx_t gid = eval_const.row_group_ids[row];
                if (gid == DConstants::INVALID_INDEX) {
                    continue;
                }
                if (!has_filter || filter_mask[row]) {
                    group_counts[gid]++;
                }
            }
            for (auto &coefficients : row_coefficients) {
                for (idx_t row = 0; row < coefficients.size(); row++) {
                    idx_t gid = eval_const.row_group_ids[row];
                    if (gid == DConstants::INVALID_INDEX || group_counts[gid] == 0) {
                        coefficients[row] = 0.0;
                        continue;
                    }
                    coefficients[row] /= std::sqrt(static_cast<double>(group_counts[gid]));
                }
            }
        };

        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            if (term_filters[term_idx].avg_scale) {
                ScaleAvgRowCoefficients(eval_const.row_coefficients[term_idx], term_filters[term_idx].has_filter,
                                        term_filters[term_idx].mask);
            }
        }

        // Evaluate bilinear terms in constraint (if any)
        if (constraint->has_bilinear) {
            for (idx_t term_idx = 0; term_idx < constraint->bilinear_terms.size(); term_idx++) {
                auto &bt = constraint->bilinear_terms[term_idx];
                EvaluatedConstraint::BilinearTerm ebt;
                ebt.var_a = bt.var_a;
                ebt.var_b = bt.var_b;

                if (bt.coefficient) {
                    auto transformed = TransformToChunkExpression(*bt.coefficient, context);
                    ExpressionExecutor coef_executor(context);
                    coef_executor.AddExpression(*transformed);
                    ColumnDataScanState bl_scan;
                    gstate.data.InitializeScan(bl_scan);
                    DataChunk bl_chunk;
                    bl_chunk.Initialize(context, gstate.data.Types());
                    while (gstate.data.Scan(bl_scan, bl_chunk)) {
                        DataChunk coef_result;
                        coef_result.Initialize(context, {transformed->return_type});
                        coef_executor.Execute(bl_chunk, coef_result);
                        auto &vec = coef_result.data[0];
                        for (idx_t r = 0; r < bl_chunk.size(); r++) {
                            Value val = vec.GetValue(r);
                            if (val.IsNull()) {
                                throw InvalidInputException(
                                    "DECIDE bilinear constraint coefficient returned NULL at row %llu.",
                                    ebt.row_coefficients.size());
                            }
                            double dval = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                            ebt.row_coefficients.push_back(dval * bt.sign);
                        }
                    }
                } else {
                    ebt.row_coefficients.assign(num_rows, static_cast<double>(bt.sign));
                }
                if (bilinear_filters[term_idx].has_filter) {
                    auto &mask = bilinear_filters[term_idx].mask;
                    for (idx_t row = 0; row < ebt.row_coefficients.size(); row++) {
                        if (!mask[row]) {
                            ebt.row_coefficients[row] = 0.0;
                        }
                    }
                }
                if (bilinear_filters[term_idx].avg_scale) {
                    ScaleAvgRowCoefficients(ebt.row_coefficients, bilinear_filters[term_idx].has_filter,
                                            bilinear_filters[term_idx].mask);
                }
                eval_const.bilinear_terms.push_back(std::move(ebt));
            }
        }

        // Evaluate quadratic groups in constraint (POWER(expr, 2) / self-products)
        if (constraint->has_quadratic) {
            eval_const.has_quadratic = true;
            for (idx_t group_idx = 0; group_idx < constraint->quadratic_groups.size(); group_idx++) {
                auto &qg = constraint->quadratic_groups[group_idx];
                EvaluatedConstraint::QuadraticGroup eqg;
                eqg.sign = qg.sign;

                for (auto &term : qg.inner_terms) {
                    eqg.variable_indices.push_back(term.variable_index);

                    auto transformed = TransformToChunkExpression(*term.coefficient, context);
                    ExpressionExecutor coef_executor(context);
                    coef_executor.AddExpression(*transformed);

                    vector<double> row_coeffs;
                    ColumnDataScanState qscan;
                    gstate.data.InitializeScan(qscan);
                    DataChunk qchunk;
                    qchunk.Initialize(context, gstate.data.Types());

                    while (gstate.data.Scan(qscan, qchunk)) {
                        DataChunk coef_result;
                        coef_result.Initialize(context, {transformed->return_type});
                        coef_executor.Execute(qchunk, coef_result);
                        auto &vec = coef_result.data[0];
                        for (idx_t r = 0; r < qchunk.size(); r++) {
                            Value val = vec.GetValue(r);
                            if (val.IsNull()) {
                                throw InvalidInputException(
                                    "DECIDE quadratic constraint coefficient returned NULL at row %llu. "
                                    "Use COALESCE() or WHERE to handle NULLs.",
                                    row_coeffs.size());
                            }
                            double dval = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                            row_coeffs.push_back(dval * term.sign);
                        }
                    }
                    eqg.row_coefficients.push_back(std::move(row_coeffs));
                }
                if (quadratic_filters[group_idx].has_filter) {
                    auto &mask = quadratic_filters[group_idx].mask;
                    for (auto &coefficients : eqg.row_coefficients) {
                        for (idx_t row = 0; row < coefficients.size(); row++) {
                            if (!mask[row]) {
                                coefficients[row] = 0.0;
                            }
                        }
                    }
                }
                if (quadratic_filters[group_idx].avg_scale) {
                    ScaleAvgQuadraticCoefficients(eqg.row_coefficients, quadratic_filters[group_idx].has_filter,
                                                  quadratic_filters[group_idx].mask);
                }
                eval_const.quadratic_groups.push_back(std::move(eqg));
            }
        }

        gstate.evaluated_constraints.push_back(std::move(eval_const));
    }

    // 2. Evaluate objective
    vector<TermFilterState> obj_linear_term_filters;
    vector<TermFilterState> obj_quadratic_term_filters;
    vector<TermFilterState> obj_bilinear_filters;
    vector<bool> objective_when_mask;
    bool objective_has_when = false;

    if (gstate.objective) {
        if (gstate.objective->has_quadratic) {
            gstate.has_quadratic_objective = true;
            gstate.quadratic_sign = gstate.objective->quadratic_sign;
        }

        objective_has_when = (gstate.objective->when_condition != nullptr);
        if (objective_has_when) {
            objective_when_mask = EvaluateBooleanMask(*gstate.objective->when_condition);
            if (objective_when_mask.size() != num_rows) {
                throw InternalException("DECIDE objective WHEN mask size mismatch: expected %llu rows, got %llu",
                                        num_rows, objective_when_mask.size());
            }
        }

        // PackDB: evaluate both the linear and quadratic-inner term lists so that
        // mixed objectives (e.g. SUM(POWER(x-t, 2) + penalty * x)) emit coefficients
        // into both solver-input arrays. The two buckets are processed together
        // inside a single scan over gstate.data — doubling coefficient evaluators
        // was cheap, but doubling the ColumnDataCollection scan was not.
        struct ObjBucket {
            vector<Term> *src_terms;
            vector<vector<double>> *out_coeffs;
            vector<idx_t> *out_var_indices;
            vector<TermFilterState> *out_term_filters;
        };
        vector<ObjBucket> buckets;
        if (!gstate.objective->terms.empty()) {
            buckets.push_back({&gstate.objective->terms,
                               &gstate.evaluated_objective_coefficients,
                               &gstate.objective_variable_indices,
                               &obj_linear_term_filters});
        }
        if (gstate.objective->has_quadratic) {
            buckets.push_back({&gstate.objective->squared_terms,
                               &gstate.evaluated_quadratic_coefficients,
                               &gstate.quadratic_variable_indices,
                               &obj_quadratic_term_filters});
        }

        if (!buckets.empty()) {
            // Per-bucket: pre-size filters + out_coeffs and snapshot variable indices.
            for (auto &b : buckets) {
                b.out_term_filters->resize(b.src_terms->size());
                b.out_coeffs->resize(b.src_terms->size());
                for (idx_t term_idx = 0; term_idx < b.src_terms->size(); term_idx++) {
                    auto &term = (*b.src_terms)[term_idx];
                    (*b.out_term_filters)[term_idx].avg_scale = term.avg_scale;
                    if (term.filter) {
                        (*b.out_term_filters)[term_idx].has_filter = true;
                        (*b.out_term_filters)[term_idx].mask = EvaluateBooleanMask(*term.filter);
                        if ((*b.out_term_filters)[term_idx].mask.size() != num_rows) {
                            throw InternalException(
                                "DECIDE objective aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                                num_rows, (*b.out_term_filters)[term_idx].mask.size());
                        }
                    }
                    b.out_var_indices->push_back(term.variable_index);
                }
            }

            // Flatten all coefficient expressions across buckets. `route[i]` names
            // the bucket and the position within that bucket that flat term `i`
            // writes to — so the scan loop can route each evaluated value back
            // without knowing which bucket it came from.
            vector<unique_ptr<Expression>> flat_coeffs;
            vector<pair<idx_t, idx_t>> route; // (bucket_idx, term_idx)
            for (idx_t b_idx = 0; b_idx < buckets.size(); b_idx++) {
                auto &b = buckets[b_idx];
                for (idx_t term_idx = 0; term_idx < b.src_terms->size(); term_idx++) {
                    flat_coeffs.push_back(
                        TransformToChunkExpression(*(*b.src_terms)[term_idx].coefficient, context));
                    route.emplace_back(b_idx, term_idx);
                }
            }

            ColumnDataScanState obj_scan_state;
            gstate.data.InitializeScan(obj_scan_state);
            DataChunk obj_chunk;
            obj_chunk.Initialize(context, gstate.data.Types());

            while (gstate.data.Scan(obj_scan_state, obj_chunk)) {
                for (idx_t j = 0; j < flat_coeffs.size(); j++) {
                    ExpressionExecutor term_executor(context);
                    term_executor.AddExpression(*flat_coeffs[j]);

                    DataChunk term_result;
                    vector<LogicalType> result_types = {flat_coeffs[j]->return_type};
                    term_result.Initialize(context, result_types);
                    term_executor.Execute(obj_chunk, term_result);

                    auto &vec = term_result.data[0];
                    auto &b = buckets[route[j].first];
                    idx_t term_idx = route[j].second;
                    auto &out = (*b.out_coeffs)[term_idx];
                    int term_sign = (*b.src_terms)[term_idx].sign;

                    for (idx_t row_in_chunk = 0; row_in_chunk < obj_chunk.size(); row_in_chunk++) {
                        Value val = vec.GetValue(row_in_chunk);
                        if (val.IsNull()) {
                            throw InvalidInputException(
                                "DECIDE objective coefficient returned NULL at row %llu. "
                                "NULL values are not allowed in optimization objective. "
                                "Use COALESCE() to handle NULLs or filter them with WHERE clause.",
                                out.size());
                        }
                        double double_val = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                        if (!std::isfinite(double_val)) {
                            throw InvalidInputException(
                                "DECIDE objective coefficient contains invalid value (NaN or Infinity) at row %llu. "
                                "Common causes:\n"
                                "  • Division by zero in objective expression\n"
                                "  • Arithmetic overflow in calculations\n"
                                "  • NULL values that propagated through math operations\n"
                                "Check your objective expressions and input data.",
                                out.size());
                        }
                        out.push_back(double_val * term_sign);
                    }
                }
            }

            // Apply per-term aggregate-local WHEN filters and the expression-level
            // WHEN mask once per bucket. Both masks are shared between linear and
            // quadratic lists for a mixed objective.
            for (auto &b : buckets) {
                for (idx_t term_idx = 0; term_idx < b.out_coeffs->size(); term_idx++) {
                    if ((*b.out_term_filters)[term_idx].has_filter) {
                        auto &mask = (*b.out_term_filters)[term_idx].mask;
                        auto &out = (*b.out_coeffs)[term_idx];
                        for (idx_t row = 0; row < out.size(); row++) {
                            if (!mask[row]) {
                                out[row] = 0.0;
                            }
                        }
                    }
                }
                if (objective_has_when) {
                    for (idx_t row = 0; row < num_rows; row++) {
                        if (objective_when_mask[row]) continue;
                        for (idx_t term_idx = 0; term_idx < b.out_coeffs->size(); term_idx++) {
                            (*b.out_coeffs)[term_idx][row] = 0.0;
                        }
                    }
                }
            }
        }

        obj_bilinear_filters.resize(gstate.objective->bilinear_terms.size());
        for (idx_t term_idx = 0; term_idx < gstate.objective->bilinear_terms.size(); term_idx++) {
            auto &term = gstate.objective->bilinear_terms[term_idx];
            obj_bilinear_filters[term_idx].avg_scale = term.avg_scale;
            if (term.filter) {
                obj_bilinear_filters[term_idx].has_filter = true;
                obj_bilinear_filters[term_idx].mask = EvaluateBooleanMask(*term.filter);
                if (obj_bilinear_filters[term_idx].mask.size() != num_rows) {
                    throw InternalException("DECIDE objective aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                                            num_rows, obj_bilinear_filters[term_idx].mask.size());
                }
            }
        }

        // No extra debug here; solver output will show timings/objective

        // Evaluate bilinear term coefficients (non-Boolean pairs left by optimizer)
        if (gstate.objective->has_bilinear) {
            for (idx_t term_idx = 0; term_idx < gstate.objective->bilinear_terms.size(); term_idx++) {
                auto &bt = gstate.objective->bilinear_terms[term_idx];
                DecideGlobalSinkState::EvaluatedBilinearTerm ebt;
                ebt.var_a = bt.var_a;
                ebt.var_b = bt.var_b;

                if (bt.coefficient) {
                    // Evaluate the data coefficient per-row
                    auto transformed = TransformToChunkExpression(*bt.coefficient, context);
                    ExpressionExecutor coef_executor(context);
                    coef_executor.AddExpression(*transformed);

                    ColumnDataScanState bl_scan;
                    gstate.data.InitializeScan(bl_scan);
                    DataChunk bl_chunk;
                    bl_chunk.Initialize(context, gstate.data.Types());

                    while (gstate.data.Scan(bl_scan, bl_chunk)) {
                        DataChunk coef_result;
                        coef_result.Initialize(context, {transformed->return_type});
                        coef_executor.Execute(bl_chunk, coef_result);
                        auto &vec = coef_result.data[0];
                        for (idx_t r = 0; r < bl_chunk.size(); r++) {
                            Value val = vec.GetValue(r);
                            if (val.IsNull()) {
                                throw InvalidInputException(
                                    "DECIDE bilinear objective coefficient returned NULL at row %llu.",
                                    ebt.row_coefficients.size());
                            }
                            double dval = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                            ebt.row_coefficients.push_back(dval * bt.sign);
                        }
                    }
                } else {
                    // Coefficient is 1.0 for all rows
                    ebt.row_coefficients.assign(num_rows, static_cast<double>(bt.sign));
                }

                if (obj_bilinear_filters[term_idx].has_filter) {
                    auto &mask = obj_bilinear_filters[term_idx].mask;
                    for (idx_t row = 0; row < ebt.row_coefficients.size(); row++) {
                        if (!mask[row]) {
                            ebt.row_coefficients[row] = 0.0;
                        }
                    }
                }

                // Apply WHEN mask to bilinear coefficients (same as linear)
                if (objective_has_when) {
                    for (idx_t row = 0; row < ebt.row_coefficients.size(); row++) {
                        if (!objective_when_mask[row]) {
                            ebt.row_coefficients[row] = 0.0;
                        }
                    }
                }

                gstate.evaluated_bilinear_terms.push_back(std::move(ebt));
            }
        }
    }

    //===--------------------------------------------------------------------===//
    // PHASE 3: Build and Solve ILP
    //===--------------------------------------------------------------------===//

    // Construct SolverInput (num_decide_vars already declared above)
    SolverInput solver_input;
    solver_input.num_rows = num_rows;
    solver_input.num_decide_vars = num_decide_vars;
    solver_input.entity_mappings = std::move(entity_mappings);
    solver_input.variable_entity_scope = variable_entity_scope;
    
    // Variable types and bounds
    solver_input.variable_types.resize(num_decide_vars);
    solver_input.lower_bounds.assign(num_decide_vars, 0.0); // Default lower
    solver_input.upper_bounds.assign(num_decide_vars, 1e30); // Default upper
    
    for (idx_t var = 0; var < num_decide_vars; var++) {
        auto &decide_var = decide_variables[var]->Cast<BoundColumnRefExpression>();
        solver_input.variable_types[var] = decide_var.return_type;
        
        // Set default bounds based on type (same logic as in solver, but good to be explicit)
        if (decide_var.return_type == LogicalType::BOOLEAN) {
            solver_input.upper_bounds[var] = 1.0;
        }
    }
    
    // Extract bounds from constraints
    gstate.ExtractVariableBounds(solver_input.lower_bounds, solver_input.upper_bounds);

    // Generate Big-M linking constraints for COUNT indicator variables.
    // Note: z <= x (forces z=0 when x=0) is now generated by DecideOptimizer
    // at plan time, flowing through normal constraint evaluation. Only the
    // M-dependent constraint x <= M*z remains here.
    for (auto &link : count_indicator_links) {
        idx_t indicator_idx = link.first;
        idx_t original_idx = link.second;

        double M = solver_input.upper_bounds[original_idx];
        if (M >= 1e20) {
            // No explicit upper bound found; use a large default
            M = 1e6;
        }

        // x <= M*z  (i.e., x - M*z <= 0) — forces z=1 when x>0
        EvaluatedConstraint ec;
        ec.variable_indices = {original_idx, indicator_idx};
        ec.row_coefficients = {vector<double>(num_rows, 1.0), vector<double>(num_rows, -M)};
        ec.rhs_values.assign(num_rows, 0.0);
        ec.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec.lhs_is_aggregate = false;
        gstate.evaluated_constraints.push_back(std::move(ec));
    }

    // Generate Big-M constraints for MIN/MAX indicator variables
    // For hard cases where MIN/MAX was rewritten to SUM by the optimizer:
    //   MAX(expr) >= K: for each row i, expr_i - M*y_i >= K - M, and SUM(y) >= 1
    //   MIN(expr) <= K: for each row i, expr_i + M*y_i <= K + M, and SUM(y) >= 1
    // Constraints are matched to their indicator variables via explicit tags (not positional).
    if (!minmax_indicator_links.empty()) {
        vector<EvaluatedConstraint> new_constraints;
        for (auto &ec : gstate.evaluated_constraints) {
            // Skip constraints without a minmax indicator tag
            if (ec.minmax_indicator_idx == DConstants::INVALID_INDEX) {
                new_constraints.push_back(std::move(ec));
                continue;
            }

            idx_t indicator_idx = ec.minmax_indicator_idx;
            bool is_max_agg = (ec.minmax_agg_type == "max");

            // Compute Big-M from variable bounds
            double M = 1e6;
            for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
                idx_t var_idx = ec.variable_indices[t];
                double ub = solver_input.upper_bounds[var_idx];
                if (ub < 1e20) {
                    double max_coef = 0.0;
                    for (auto &v : ec.row_coefficients[t]) {
                        max_coef = std::max(max_coef, std::abs(v));
                    }
                    M = std::max(M, max_coef * ub);
                }
            }

            if (is_max_agg) {
                // Hard MAX(expr) >= K: for each row i, expr_i - M*y_i >= K - M
                // This is a per-row constraint (not aggregate)
                EvaluatedConstraint ec_row;
                ec_row.variable_indices = ec.variable_indices;
                ec_row.row_coefficients = ec.row_coefficients;
                // Add indicator variable: -M * y_i
                ec_row.variable_indices.push_back(indicator_idx);
                ec_row.row_coefficients.push_back(vector<double>(num_rows, -M));
                ec_row.rhs_values.resize(num_rows);
                for (idx_t r = 0; r < num_rows; r++) {
                    ec_row.rhs_values[r] = ec.rhs_values[r] - M;
                }
                ec_row.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
                ec_row.lhs_is_aggregate = false; // per-row!
                ec_row.row_group_ids = ec.row_group_ids;
                ec_row.num_groups = ec.num_groups;
                ec_row.per_strict = ec.per_strict;
                new_constraints.push_back(std::move(ec_row));

                // SUM(y) >= 1 (at least one row must satisfy)
                EvaluatedConstraint ec_sum;
                ec_sum.variable_indices = {indicator_idx};
                ec_sum.row_coefficients = {vector<double>(num_rows, 1.0)};
                ec_sum.rhs_values.assign(num_rows, 1.0);
                ec_sum.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
                ec_sum.lhs_is_aggregate = true;
                ec_sum.row_group_ids = ec.row_group_ids;
                ec_sum.num_groups = ec.num_groups;
                ec_sum.per_strict = ec.per_strict;
                new_constraints.push_back(std::move(ec_sum));
            } else {
                // MIN(expr) <= K: for each row i, expr_i + M*y_i <= K + M
                EvaluatedConstraint ec_row;
                ec_row.variable_indices = ec.variable_indices;
                ec_row.row_coefficients = ec.row_coefficients;
                // Add indicator variable: +M * y_i
                ec_row.variable_indices.push_back(indicator_idx);
                ec_row.row_coefficients.push_back(vector<double>(num_rows, M));
                ec_row.rhs_values.resize(num_rows);
                for (idx_t r = 0; r < num_rows; r++) {
                    ec_row.rhs_values[r] = ec.rhs_values[r] + M;
                }
                ec_row.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
                ec_row.lhs_is_aggregate = false;
                ec_row.row_group_ids = ec.row_group_ids;
                ec_row.num_groups = ec.num_groups;
                ec_row.per_strict = ec.per_strict;
                new_constraints.push_back(std::move(ec_row));

                // SUM(y) >= 1
                EvaluatedConstraint ec_sum;
                ec_sum.variable_indices = {indicator_idx};
                ec_sum.row_coefficients = {vector<double>(num_rows, 1.0)};
                ec_sum.rhs_values.assign(num_rows, 1.0);
                ec_sum.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
                ec_sum.lhs_is_aggregate = true;
                ec_sum.row_group_ids = ec.row_group_ids;
                ec_sum.num_groups = ec.num_groups;
                ec_sum.per_strict = ec.per_strict;
                new_constraints.push_back(std::move(ec_sum));
            }
        }
        gstate.evaluated_constraints = std::move(new_constraints);
    }

    // Generate Big-M constraints for not-equal (<>) indicators.
    // For each COMPARE_NOTEQUAL constraint, replace it with two disjunctive constraints:
    //   x - M*z ≤ K-1        (z=0 → x ≤ K-1; z=1 → trivially true)
    //   x - M*z ≥ K+1-M      (z=0 → trivially true; z=1 → x ≥ K+1)
    //
    // Per-row NE: expanded inline with row-scoped indicator variables (one z per row).
    // Aggregate NE: deferred — expanded after the VarIndexer is built, using a single
    //   global binary z per group. This avoids the per-row z interaction with the
    //   aggregate constraint building path (unified path with row_group_ids).
    struct DeferredAggregateNE {
        EvaluatedConstraint original;
        double big_M;
    };
    vector<DeferredAggregateNE> deferred_ne_aggregate;

    // The ±1 band above is only semantically exact when the LHS is integer-valued.
    // For REAL variables or non-integer coefficients the band (K-1, K+1) wrongly
    // excludes feasible continuous points. Mirror the strict-inequality guard in
    // ilp_model_builder.cpp::IsEvalConstraintLhsIntegerValued.
    auto NEIsRealType = [](const LogicalType &t) {
        return t == LogicalType::DOUBLE || t == LogicalType::FLOAT;
    };
    auto NEAllCoeffsIntegral = [](const vector<double> &coeffs) {
        for (double c : coeffs) {
            if (std::floor(c) != c) return false;
        }
        return true;
    };
    auto NELhsIsIntegerValued = [&](const EvaluatedConstraint &ec) -> bool {
        for (idx_t i = 0; i < ec.variable_indices.size(); i++) {
            idx_t vi = ec.variable_indices[i];
            if (vi == DConstants::INVALID_INDEX) continue;
            if (NEIsRealType(solver_input.variable_types[vi])) return false;
            if (!NEAllCoeffsIntegral(ec.row_coefficients[i])) return false;
        }
        return true;
    };

    if (!ne_indicator_indices.empty()) {
        vector<EvaluatedConstraint> new_constraints;
        for (auto &ec : gstate.evaluated_constraints) {
            if (ec.ne_indicator_idx != DConstants::INVALID_INDEX) {
                if (!NELhsIsIntegerValued(ec)) {
                    throw InvalidInputException(
                        "Inequality '<>' is not supported when the left-hand side "
                        "involves a REAL variable or a non-integer coefficient. "
                        "The integer-step rewrite (x <> K → x <= K-1 OR x >= K+1) "
                        "would cut continuous feasible points in the band (K-1, K+1).");
                }
                // Compute M from variable bounds (only consider active rows)
                double M = 1e6; // default
                for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
                    idx_t var_idx = ec.variable_indices[t];
                    double ub = solver_input.upper_bounds[var_idx];
                    if (ub < 1e20) {
                        double max_coef = 0.0;
                        for (idx_t r = 0; r < ec.row_coefficients[t].size(); r++) {
                            if (!ec.row_group_ids.empty() &&
                                ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
                                continue;
                            }
                            max_coef = std::max(max_coef, std::abs(ec.row_coefficients[t][r]));
                        }
                        M = std::max(M, max_coef * ub);
                    }
                }

                if (ec.lhs_is_aggregate) {
                    // Aggregate NE: defer to after pre_indexer is built.
                    // Will be expanded with a single global z per group.
                    DeferredAggregateNE deferred;
                    deferred.original = ec; // copy before the loop moves on
                    deferred.big_M = M;
                    deferred_ne_aggregate.push_back(std::move(deferred));
                    // Don't add to new_constraints — handled via global_constraints later
                } else {
                    // Per-row NE: expand inline with row-scoped indicator variable
                    idx_t indicator_var_idx = ec.ne_indicator_idx;

                    // Build indicator coefficient vector (0 for excluded rows, -M for active)
                    vector<double> indicator_coeffs(num_rows, 0.0);
                    for (idx_t r = 0; r < num_rows; r++) {
                        if (ec.row_group_ids.empty() ||
                            ec.row_group_ids[r] != DConstants::INVALID_INDEX) {
                            indicator_coeffs[r] = -M;
                        }
                    }

                    // Constraint 1: x - M*z ≤ K - 1
                    EvaluatedConstraint ec1;
                    ec1.variable_indices = ec.variable_indices;
                    ec1.row_coefficients = ec.row_coefficients;
                    ec1.variable_indices.push_back(indicator_var_idx);
                    ec1.row_coefficients.push_back(indicator_coeffs);
                    ec1.rhs_values.resize(num_rows);
                    for (idx_t r = 0; r < num_rows; r++) {
                        ec1.rhs_values[r] = ec.rhs_values[r] - 1.0;
                    }
                    ec1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
                    ec1.lhs_is_aggregate = false; // per-row
                    ec1.row_group_ids = ec.row_group_ids;
                    ec1.num_groups = ec.num_groups;
                    ec1.per_strict = ec.per_strict;
                    new_constraints.push_back(std::move(ec1));

                    // Constraint 2: x - M*z ≥ K + 1 - M
                    EvaluatedConstraint ec2;
                    ec2.variable_indices = ec.variable_indices;
                    ec2.row_coefficients = ec.row_coefficients;
                    ec2.variable_indices.push_back(indicator_var_idx);
                    ec2.row_coefficients.push_back(indicator_coeffs);
                    ec2.rhs_values.resize(num_rows);
                    for (idx_t r = 0; r < num_rows; r++) {
                        ec2.rhs_values[r] = ec.rhs_values[r] + 1.0 - M;
                    }
                    ec2.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
                    ec2.lhs_is_aggregate = false; // per-row
                    ec2.row_group_ids = ec.row_group_ids;
                    ec2.num_groups = ec.num_groups;
                    ec2.per_strict = ec.per_strict;
                    new_constraints.push_back(std::move(ec2));
                }
            } else {
                new_constraints.push_back(std::move(ec));
            }
        }
        gstate.evaluated_constraints = std::move(new_constraints);
    }

    // Generate McCormick Big-M constraints for bilinear auxiliary variables (w = b * x).
    // The structural constraint w <= x was generated at optimizer time.
    // Here we add: w <= U*b and w >= x - U*(1-b), which require the execution-time bound U.
    for (auto &link : bilinear_links) {
        double U = solver_input.upper_bounds[link.other_var_idx];
        if (U >= 1e20) {
            throw InvalidInputException(
                "Bilinear term requires a finite upper bound on variable '%s'. "
                "Add a constraint like '%s <= <bound>' to provide one.",
                decide_variables[link.other_var_idx]->Cast<BoundColumnRefExpression>().alias,
                decide_variables[link.other_var_idx]->Cast<BoundColumnRefExpression>().alias);
        }

        // Constraint: w <= U * b  (i.e., w - U*b <= 0)
        EvaluatedConstraint ec1;
        ec1.variable_indices = {link.aux_idx, link.bool_var_idx};
        ec1.row_coefficients = {vector<double>(num_rows, 1.0), vector<double>(num_rows, -U)};
        ec1.rhs_values.assign(num_rows, 0.0);
        ec1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
        ec1.lhs_is_aggregate = false;
        gstate.evaluated_constraints.push_back(std::move(ec1));

        // Constraint: w >= x - U*(1-b) = x - U + U*b
        // Rearranged: w - x + U*b >= -U  →  1*w + (-1)*x + (-U)*b >= -U
        EvaluatedConstraint ec2;
        ec2.variable_indices = {link.aux_idx, link.other_var_idx, link.bool_var_idx};
        ec2.row_coefficients = {
            vector<double>(num_rows, 1.0),     // +w
            vector<double>(num_rows, -1.0),    // -x
            vector<double>(num_rows, -U)       // -U*b
        };
        ec2.rhs_values.assign(num_rows, -U);   // RHS = -U
        ec2.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
        ec2.lhs_is_aggregate = false;
        gstate.evaluated_constraints.push_back(std::move(ec2));
    }

    // Constraints
    solver_input.constraints = std::move(gstate.evaluated_constraints);

    // Objective (linear part)
    solver_input.objective_coefficients = std::move(gstate.evaluated_objective_coefficients);
    solver_input.objective_variable_indices = std::move(gstate.objective_variable_indices);
    solver_input.sense = decide_sense;

    // Quadratic objective (if present)
    if (gstate.has_quadratic_objective) {
        solver_input.has_quadratic_objective = true;
        solver_input.quadratic_sign = gstate.quadratic_sign;
        solver_input.quadratic_inner_coefficients = std::move(gstate.evaluated_quadratic_coefficients);
        solver_input.quadratic_inner_variable_indices.resize(gstate.quadratic_variable_indices.size());
        for (idx_t i = 0; i < gstate.quadratic_variable_indices.size(); i++) {
            solver_input.quadratic_inner_variable_indices[i] = gstate.quadratic_variable_indices[i];
        }
    }

    // Bilinear objective terms (non-Boolean pairs, for Q matrix off-diagonal entries)
    if (!gstate.evaluated_bilinear_terms.empty()) {
        for (auto &ebt : gstate.evaluated_bilinear_terms) {
            SolverInput::BilinearObjectiveTerm bot;
            bot.var_a = ebt.var_a;
            bot.var_b = ebt.var_b;
            bot.row_coefficients = std::move(ebt.row_coefficients);
            solver_input.bilinear_objective_terms.push_back(std::move(bot));
        }
    }

    // Evaluate PER column for objective grouping (must happen after solver_input is constructed)
    if (gstate.objective && !gstate.objective->per_columns.empty()) {
        bool objective_has_local_filters = false;
        bool objective_has_unfiltered_part = false;
        for (auto &f : obj_linear_term_filters) {
            objective_has_local_filters |= f.has_filter;
            objective_has_unfiltered_part |= !f.has_filter;
        }
        for (auto &f : obj_quadratic_term_filters) {
            objective_has_local_filters |= f.has_filter;
            objective_has_unfiltered_part |= !f.has_filter;
        }
        for (auto &f : obj_bilinear_filters) {
            objective_has_local_filters |= f.has_filter;
            objective_has_unfiltered_part |= !f.has_filter;
        }

        auto objective_row_has_local_term = [&](idx_t row) {
            if (!objective_has_local_filters || objective_has_unfiltered_part) {
                return true;
            }
            for (auto &f : obj_linear_term_filters) {
                if (f.has_filter && f.mask[row]) {
                    return true;
                }
            }
            for (auto &f : obj_quadratic_term_filters) {
                if (f.has_filter && f.mask[row]) {
                    return true;
                }
            }
            for (auto &f : obj_bilinear_filters) {
                if (f.has_filter && f.mask[row]) {
                    return true;
                }
            }
            return false;
        };

        // Evaluate PER columns
        vector<vector<Value>> obj_per_values;
        obj_per_values.resize(gstate.objective->per_columns.size());
        for (idx_t col_idx = 0; col_idx < gstate.objective->per_columns.size(); col_idx++) {
            auto transformed_col = TransformToChunkExpression(*gstate.objective->per_columns[col_idx], context);
            ExpressionExecutor per_executor(context);
            per_executor.AddExpression(*transformed_col);
            obj_per_values[col_idx].reserve(num_rows);
            ColumnDataScanState per_scan;
            gstate.data.InitializeScan(per_scan);
            DataChunk per_chunk;
            per_chunk.Initialize(context, gstate.data.Types());
            while (gstate.data.Scan(per_scan, per_chunk)) {
                DataChunk per_result;
                per_result.Initialize(context, {transformed_col->return_type});
                per_executor.Execute(per_chunk, per_result);
                auto &vec = per_result.data[0];
                for (idx_t r = 0; r < per_chunk.size(); r++) {
                    obj_per_values[col_idx].push_back(vec.GetValue(r));
                }
            }
        }

        // Build unified row_group_ids for objective
        solver_input.objective_row_group_ids.resize(num_rows);
        unordered_map<string, idx_t> obj_value_to_group;
        idx_t obj_next_group = 0;
        bool obj_strict = gstate.objective->per_strict;

        // Helper: build composite PER key for objective
        auto build_obj_per_key = [&](idx_t row, bool &has_null_out) -> string {
            has_null_out = false;
            for (idx_t col_idx = 0; col_idx < obj_per_values.size(); col_idx++) {
                if (obj_per_values[col_idx][row].IsNull()) {
                    has_null_out = true;
                    return {};
                }
            }
            string key;
            for (idx_t col_idx = 0; col_idx < obj_per_values.size(); col_idx++) {
                if (col_idx > 0) key.push_back('\0');
                key += obj_per_values[col_idx][row].ToString();
            }
            return key;
        };

        auto obj_row_is_included = [&](idx_t row) -> bool {
            if (objective_has_when && !objective_when_mask[row]) return false;
            if (!objective_row_has_local_term(row)) return false;
            return true;
        };

        if (obj_strict) {
            // PER STRICT: Phase 1 — discover groups from ALL rows (ignoring WHEN)
            for (idx_t row = 0; row < num_rows; row++) {
                bool has_null = false;
                string key = build_obj_per_key(row, has_null);
                if (has_null) continue;
                if (obj_value_to_group.find(key) == obj_value_to_group.end()) {
                    obj_value_to_group[key] = obj_next_group++;
                }
            }
            // Phase 2: Assign row_group_ids (WHEN-excluded → INVALID_INDEX)
            for (idx_t row = 0; row < num_rows; row++) {
                bool has_null = false;
                string key = build_obj_per_key(row, has_null);
                if (has_null || !obj_row_is_included(row)) {
                    solver_input.objective_row_group_ids[row] = DConstants::INVALID_INDEX;
                    continue;
                }
                solver_input.objective_row_group_ids[row] = obj_value_to_group[key];
            }
        } else {
            // Standard WHEN→PER: discover groups only from qualifying rows
            for (idx_t row = 0; row < num_rows; row++) {
                if (!obj_row_is_included(row)) {
                    solver_input.objective_row_group_ids[row] = DConstants::INVALID_INDEX;
                    continue;
                }
                bool has_null = false;
                string key = build_obj_per_key(row, has_null);
                if (has_null) {
                    solver_input.objective_row_group_ids[row] = DConstants::INVALID_INDEX;
                    continue;
                }
                auto it = obj_value_to_group.find(key);
                if (it == obj_value_to_group.end()) {
                    obj_value_to_group[key] = obj_next_group;
                    solver_input.objective_row_group_ids[row] = obj_next_group;
                    obj_next_group++;
                } else {
                    solver_input.objective_row_group_ids[row] = it->second;
                }
            }
        }
        solver_input.objective_num_groups = obj_next_group;
    }

    auto ScaleObjectiveAvgRows = [&](vector<double> &coefficients, bool has_filter, const vector<bool> &filter_mask,
                                     bool quadratic_inner) {
        if (solver_input.objective_row_group_ids.empty()) {
            idx_t denominator = 0;
            for (idx_t row = 0; row < num_rows; row++) {
                if (objective_has_when && !objective_when_mask[row]) {
                    continue;
                }
                if (!has_filter || filter_mask[row]) {
                    denominator++;
                }
            }
            if (denominator == 0) {
                std::fill(coefficients.begin(), coefficients.end(), 0.0);
                return;
            }
            double scale = quadratic_inner ? 1.0 / std::sqrt(static_cast<double>(denominator))
                                           : 1.0 / static_cast<double>(denominator);
            for (auto &coefficient : coefficients) {
                coefficient *= scale;
            }
            return;
        }

        vector<idx_t> group_counts(solver_input.objective_num_groups, 0);
        for (idx_t row = 0; row < num_rows; row++) {
            idx_t gid = solver_input.objective_row_group_ids[row];
            if (gid == DConstants::INVALID_INDEX) {
                continue;
            }
            if (!has_filter || filter_mask[row]) {
                group_counts[gid]++;
            }
        }
        for (idx_t row = 0; row < coefficients.size(); row++) {
            idx_t gid = solver_input.objective_row_group_ids[row];
            if (gid == DConstants::INVALID_INDEX || group_counts[gid] == 0) {
                coefficients[row] = 0.0;
                continue;
            }
            double scale = quadratic_inner ? 1.0 / std::sqrt(static_cast<double>(group_counts[gid]))
                                           : 1.0 / static_cast<double>(group_counts[gid]);
            coefficients[row] *= scale;
        }
    };

    for (idx_t term_idx = 0; term_idx < obj_linear_term_filters.size(); term_idx++) {
        if (!obj_linear_term_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleObjectiveAvgRows(solver_input.objective_coefficients[term_idx],
                              obj_linear_term_filters[term_idx].has_filter,
                              obj_linear_term_filters[term_idx].mask, false);
    }
    for (idx_t term_idx = 0; term_idx < obj_quadratic_term_filters.size(); term_idx++) {
        if (!obj_quadratic_term_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleObjectiveAvgRows(solver_input.quadratic_inner_coefficients[term_idx],
                              obj_quadratic_term_filters[term_idx].has_filter,
                              obj_quadratic_term_filters[term_idx].mask, true);
    }

    for (idx_t term_idx = 0; term_idx < obj_bilinear_filters.size(); term_idx++) {
        if (!obj_bilinear_filters[term_idx].avg_scale) {
            continue;
        }
        ScaleObjectiveAvgRows(solver_input.bilinear_objective_terms[term_idx].row_coefficients,
                              obj_bilinear_filters[term_idx].has_filter,
                              obj_bilinear_filters[term_idx].mask, false);
    }

    // Handle MIN/MAX objective: create global auxiliary variable z and linking constraints.
    // Two paths: (A) non-PER flat MIN/MAX, (B) PER with nested OUTER(INNER(expr)).
    //
    // For PER objectives, two-level auxiliary formulation:
    //   Phase A (inner): per-group auxiliary z_g for inner MIN/MAX aggregate
    //   Phase B (outer): global auxiliary w for outer MIN/MAX aggregate
    //
    // Easy/hard classification at each level:
    //   Easy (no indicators): MINIMIZE+MAX or MAXIMIZE+MIN
    //   Hard (Big-M indicators): MINIMIZE+MIN or MAXIMIZE+MAX

    // Build a preliminary VarIndexer for computing absolute variable indices
    // in the MIN/MAX objective constraint generation below.
    // Use BuildRef — solver_input is alive for the duration of this scope.
    VarIndexer pre_indexer = VarIndexer::BuildRef(solver_input);

    // Global variables are appended at pre_indexer.global_block_start.
    // As we add more global vars, their indices are global_block_start + g
    // where g is the position in the global vars array.

    // Expand deferred aggregate NE constraints using global binary indicator variables.
    // Each group gets a single z (binary), yielding two raw constraints:
    //   SUM(coeffs) - M*z <= K-1       (z=0 → SUM ≤ K-1; z=1 → trivially true)
    //   SUM(coeffs) - M*z >= K+1-M     (z=0 → trivially true; z=1 → SUM ≥ K+1)
    for (auto &deferred : deferred_ne_aggregate) {
        auto &ec = deferred.original;
        double M = deferred.big_M;
        bool has_groups = !ec.row_group_ids.empty();

        // Build group → rows mapping (mirrors model builder unified path)
        idx_t num_groups_to_process = 1;
        vector<vector<idx_t>> group_rows;
        if (has_groups) {
            group_rows.resize(ec.num_groups);
            num_groups_to_process = ec.num_groups;
            for (idx_t row = 0; row < num_rows; row++) {
                idx_t gid = ec.row_group_ids[row];
                if (gid != DConstants::INVALID_INDEX) {
                    group_rows[gid].push_back(row);
                }
            }
        } else {
            // No WHEN/PER — all rows in one implicit group
            group_rows.resize(1);
            for (idx_t row = 0; row < num_rows; row++) {
                group_rows[0].push_back(row);
            }
        }

        double rhs = ec.rhs_values[0];

        for (idx_t g = 0; g < num_groups_to_process; g++) {
            if (group_rows[g].empty()) {
                if (!ec.per_strict) {
                    continue;
                }
                // PER STRICT + NE + empty group: SUM(∅) <> K means 0 <> K
                // If K != 0: trivially true (no constraint needed)
                // If K == 0: infeasible — emit 0 >= 1
                if (std::abs(rhs) < 1e-15) {
                    SolverInput::RawConstraint rc;
                    rc.sense = '>';
                    rc.rhs = 1.0;
                    solver_input.global_constraints.push_back(std::move(rc));
                }
                continue;
            }

            // Allocate one global binary z for this group
            idx_t z_idx = pre_indexer.global_block_start + solver_input.num_global_vars;
            solver_input.num_global_vars++;
            solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
            solver_input.global_lower_bounds.push_back(0.0);
            solver_input.global_upper_bounds.push_back(1.0);
            solver_input.global_obj_coeffs.push_back(0.0);

            // Accumulate LHS coefficients for active rows in this group
            std::unordered_map<int, double> coeff_accum;
            for (idx_t term_idx = 0; term_idx < ec.variable_indices.size(); term_idx++) {
                idx_t decide_var_idx = ec.variable_indices[term_idx];
                if (decide_var_idx == DConstants::INVALID_INDEX) {
                    continue;
                }
                for (idx_t row : group_rows[g]) {
                    double coeff = ec.row_coefficients[term_idx][row];
                    if (std::abs(coeff) < 1e-15) {
                        continue;
                    }
                    int var_idx = static_cast<int>(pre_indexer.Get(decide_var_idx, row));
                    coeff_accum[var_idx] += coeff;
                }
            }

            // ec1: SUM(coeffs) - M*z <= K - 1
            SolverInput::RawConstraint rc1;
            rc1.sense = '<';
            rc1.rhs = rhs - 1.0;
            for (auto &[idx, coeff] : coeff_accum) {
                if (coeff != 0.0) {
                    rc1.indices.push_back(idx);
                    rc1.coefficients.push_back(coeff);
                }
            }
            rc1.indices.push_back(static_cast<int>(z_idx));
            rc1.coefficients.push_back(-M);
            solver_input.global_constraints.push_back(std::move(rc1));

            // ec2: SUM(coeffs) - M*z >= K + 1 - M
            SolverInput::RawConstraint rc2;
            rc2.sense = '>';
            rc2.rhs = rhs + 1.0 - M;
            for (auto &[idx, coeff] : coeff_accum) {
                if (coeff != 0.0) {
                    rc2.indices.push_back(idx);
                    rc2.coefficients.push_back(coeff);
                }
            }
            rc2.indices.push_back(static_cast<int>(z_idx));
            rc2.coefficients.push_back(-M);
            solver_input.global_constraints.push_back(std::move(rc2));
        }
    }

    // Save objective data (needed for constraint generation in both paths)
    auto saved_obj_coefficients = solver_input.objective_coefficients;
    auto saved_obj_var_indices = solver_input.objective_variable_indices;

    // Compute Big-M from variable bounds (shared by both paths)
    auto compute_big_m = [&]() -> double {
        double M = 1e6;
        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
            idx_t var = saved_obj_var_indices[t];
            double ub = solver_input.upper_bounds[var];
            if (ub < 1e20) {
                double max_coef = 0.0;
                for (auto &v : saved_obj_coefficients[t]) {
                    max_coef = std::max(max_coef, std::abs(v));
                }
                M = std::max(M, max_coef * ub);
            }
        }
        return M;
    };

    if (per_inner_agg != ObjectiveAggregateType::NONE && !saved_obj_var_indices.empty() &&
        solver_input.objective_num_groups > 0) {
        // ================================================================
        // PATH B: PER objective with nested OUTER(INNER(expr)) aggregate
        // ================================================================
        idx_t K = solver_input.objective_num_groups;
        auto &row_groups = solver_input.objective_row_group_ids;

        // Build group→rows index
        vector<vector<idx_t>> group_rows(K);
        for (idx_t row = 0; row < num_rows; row++) {
            if (row_groups[row] != DConstants::INVALID_INDEX) {
                group_rows[row_groups[row]].push_back(row);
            }
        }

        // Clear per-row objective (auxiliaries become the objective)
        solver_input.objective_coefficients.clear();
        solver_input.objective_variable_indices.clear();

        // Phase A: Inner aggregate — produces K per-group values
        // These are either group sums (no aux) or z_g auxiliaries (inner MIN/MAX)
        bool inner_is_minmax = (per_inner_agg == ObjectiveAggregateType::MIN_AGG || per_inner_agg == ObjectiveAggregateType::MAX_AGG);
        bool inner_is_min = (per_inner_agg == ObjectiveAggregateType::MIN_AGG);

        // group_value_indices[g] = solver variable index for group g's value
        // For inner SUM: not used (group sums go directly to outer as coefficients)
        // For inner MIN/MAX: index of z_g global variable
        vector<idx_t> group_value_indices(K);

        if (inner_is_minmax) {
            // Inner MIN/MAX: create z_g auxiliary per group
            bool inner_easy = per_inner_is_easy;
            double M = compute_big_m();

            idx_t z_base = pre_indexer.global_block_start + solver_input.num_global_vars;
            for (idx_t g = 0; g < K; g++) {
                group_value_indices[g] = z_base + g;
                solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
                solver_input.global_lower_bounds.push_back(-1e30);
                solver_input.global_upper_bounds.push_back(1e30);
                solver_input.global_obj_coeffs.push_back(0.0); // set by outer phase
            }
            solver_input.num_global_vars += K;

            if (inner_easy) {
                // Easy: z_g >= expr_r (for MAX) or z_g <= expr_r (for MIN)
                char sense_char = inner_is_min ? '<' : '>';
                for (idx_t g = 0; g < K; g++) {
                    for (idx_t row : group_rows[g]) {
                        SolverInput::RawConstraint rc;
                        rc.sense = sense_char;
                        rc.rhs = 0.0;
                        rc.indices.push_back((int)group_value_indices[g]);
                        rc.coefficients.push_back(1.0);
                        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                            double coeff = saved_obj_coefficients[t][row];
                            if (std::abs(coeff) < 1e-15) continue;
                            idx_t var_idx = pre_indexer.Get(saved_obj_var_indices[t], row);
                            rc.indices.push_back((int)var_idx);
                            rc.coefficients.push_back(-coeff);
                        }
                        solver_input.global_constraints.push_back(std::move(rc));
                    }
                }
            } else {
                // Hard: per-row indicators per group
                idx_t first_y = z_base + K;
                idx_t total_indicators = num_rows; // one per row (only grouped rows used)
                solver_input.num_global_vars += total_indicators;
                for (idx_t r = 0; r < total_indicators; r++) {
                    solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
                    solver_input.global_lower_bounds.push_back(0.0);
                    solver_input.global_upper_bounds.push_back(1.0);
                    solver_input.global_obj_coeffs.push_back(0.0);
                }

                for (idx_t g = 0; g < K; g++) {
                    for (idx_t row : group_rows[g]) {
                        SolverInput::RawConstraint rc;
                        rc.indices.push_back((int)group_value_indices[g]);
                        rc.coefficients.push_back(1.0);
                        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                            double coeff = saved_obj_coefficients[t][row];
                            if (std::abs(coeff) < 1e-15) continue;
                            idx_t var_idx = pre_indexer.Get(saved_obj_var_indices[t], row);
                            rc.indices.push_back((int)var_idx);
                            rc.coefficients.push_back(-coeff);
                        }
                        idx_t y_idx = first_y + row;
                        if (inner_is_min) {
                            // MINIMIZE MIN inner: z_g - expr_r - M*y_r >= -M
                            rc.indices.push_back((int)y_idx);
                            rc.coefficients.push_back(-M);
                            rc.sense = '>';
                            rc.rhs = -M;
                        } else {
                            // MAXIMIZE MAX inner: z_g - expr_r + M*y_r <= M
                            rc.indices.push_back((int)y_idx);
                            rc.coefficients.push_back(M);
                            rc.sense = '<';
                            rc.rhs = M;
                        }
                        solver_input.global_constraints.push_back(std::move(rc));
                    }
                    // SUM(y) >= 1 per group
                    SolverInput::RawConstraint sum_y;
                    for (idx_t row : group_rows[g]) {
                        sum_y.indices.push_back((int)(first_y + row));
                        sum_y.coefficients.push_back(1.0);
                    }
                    sum_y.sense = '>';
                    sum_y.rhs = 1.0;
                    solver_input.global_constraints.push_back(std::move(sum_y));
                }
            }
        }

        // Phase B: Outer aggregate — combines K group values into scalar objective
        bool outer_is_sum = (per_outer_agg == ObjectiveAggregateType::SUM);
        bool outer_is_minmax = (per_outer_agg == ObjectiveAggregateType::MIN_AGG || per_outer_agg == ObjectiveAggregateType::MAX_AGG);
        bool outer_is_min = (per_outer_agg == ObjectiveAggregateType::MIN_AGG);

        if (inner_is_minmax && outer_is_sum) {
            // Outer SUM: objective = sum of z_g's
            for (idx_t g = 0; g < K; g++) {
                solver_input.global_obj_coeffs[group_value_indices[g] - pre_indexer.global_block_start] = 1.0;
            }
        } else if (inner_is_minmax && outer_is_minmax) {
            // Outer MIN/MAX over z_g's: create global w auxiliary
            bool outer_easy = per_outer_is_easy;

            idx_t w_idx = pre_indexer.global_block_start + solver_input.num_global_vars;
            solver_input.num_global_vars += 1;
            solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
            solver_input.global_lower_bounds.push_back(-1e30);
            solver_input.global_upper_bounds.push_back(1e30);
            solver_input.global_obj_coeffs.push_back(1.0); // objective = w

            if (outer_easy) {
                // w >= z_g (for outer MAX) or w <= z_g (for outer MIN)
                char sense_char = outer_is_min ? '<' : '>';
                for (idx_t g = 0; g < K; g++) {
                    SolverInput::RawConstraint rc;
                    rc.sense = sense_char;
                    rc.rhs = 0.0;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    rc.indices.push_back((int)group_value_indices[g]);
                    rc.coefficients.push_back(-1.0);
                    solver_input.global_constraints.push_back(std::move(rc));
                }
            } else {
                // Hard outer: indicators over K groups
                idx_t first_u = w_idx + 1;
                solver_input.num_global_vars += K;
                for (idx_t g = 0; g < K; g++) {
                    solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
                    solver_input.global_lower_bounds.push_back(0.0);
                    solver_input.global_upper_bounds.push_back(1.0);
                    solver_input.global_obj_coeffs.push_back(0.0);
                }
                // Big-M for outer: use a large value (z_g's are bounded by inner M)
                double M_outer = compute_big_m();
                for (idx_t g = 0; g < K; g++) {
                    SolverInput::RawConstraint rc;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    rc.indices.push_back((int)group_value_indices[g]);
                    rc.coefficients.push_back(-1.0);
                    idx_t u_idx = first_u + g;
                    if (outer_is_min) {
                        // MINIMIZE MIN outer: w - z_g - M*u_g >= -M
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(-M_outer);
                        rc.sense = '>';
                        rc.rhs = -M_outer;
                    } else {
                        // MAXIMIZE MAX outer: w - z_g + M*u_g <= M
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(M_outer);
                        rc.sense = '<';
                        rc.rhs = M_outer;
                    }
                    solver_input.global_constraints.push_back(std::move(rc));
                }
                SolverInput::RawConstraint sum_u;
                for (idx_t g = 0; g < K; g++) {
                    sum_u.indices.push_back((int)(first_u + g));
                    sum_u.coefficients.push_back(1.0);
                }
                sum_u.sense = '>';
                sum_u.rhs = 1.0;
                solver_input.global_constraints.push_back(std::move(sum_u));
            }
        } else if (!inner_is_minmax && outer_is_sum) {
            if (per_inner_was_avg) {
                // Inner AVG + Outer SUM: scale each row's coefficient by 1/n_g
                // SUM over groups of AVG(expr) = Σ_g (Σ_{r∈g} c_r * x_r) / n_g
                for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                    for (idx_t row = 0; row < num_rows; row++) {
                        if (row_groups[row] != DConstants::INVALID_INDEX) {
                            idx_t g = row_groups[row];
                            saved_obj_coefficients[t][row] /= static_cast<double>(group_rows[g].size());
                        }
                    }
                }
            }
            // Restore (possibly scaled) objective coefficients
            solver_input.objective_coefficients = std::move(saved_obj_coefficients);
            solver_input.objective_variable_indices = std::move(saved_obj_var_indices);
        } else if (!inner_is_minmax && outer_is_minmax) {
            // Inner SUM + Outer MIN/MAX: compute per-group sums, then optimize over them
            // Create w auxiliary for outer MIN/MAX over group sums
            bool outer_easy = per_outer_is_easy;

            idx_t w_idx = pre_indexer.global_block_start + solver_input.num_global_vars;
            solver_input.num_global_vars += 1;
            solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
            solver_input.global_lower_bounds.push_back(-1e30);
            solver_input.global_upper_bounds.push_back(1e30);
            solver_input.global_obj_coeffs.push_back(1.0); // objective = w

            // For each group g: w >= (or <=) sum_g(coeffs * x)
            // sum_g = Σ_{r ∈ group_g} Σ_t coeff_t_r * x_{r,var_t}
            if (outer_easy) {
                char sense_char = outer_is_min ? '<' : '>';
                for (idx_t g = 0; g < K; g++) {
                    SolverInput::RawConstraint rc;
                    rc.sense = sense_char;
                    rc.rhs = 0.0;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    for (idx_t row : group_rows[g]) {
                        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                            double coeff = saved_obj_coefficients[t][row];
                            if (per_inner_was_avg) {
                                coeff /= static_cast<double>(group_rows[g].size());
                            }
                            if (std::abs(coeff) < 1e-15) continue;
                            idx_t var_idx = pre_indexer.Get(saved_obj_var_indices[t], row);
                            rc.indices.push_back((int)var_idx);
                            rc.coefficients.push_back(-coeff);
                        }
                    }
                    solver_input.global_constraints.push_back(std::move(rc));
                }
            } else {
                // Hard outer: indicators over K groups
                double M_outer = compute_big_m() * num_rows; // group sums can be large
                idx_t first_u = w_idx + 1;
                solver_input.num_global_vars += K;
                for (idx_t g = 0; g < K; g++) {
                    solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
                    solver_input.global_lower_bounds.push_back(0.0);
                    solver_input.global_upper_bounds.push_back(1.0);
                    solver_input.global_obj_coeffs.push_back(0.0);
                }
                for (idx_t g = 0; g < K; g++) {
                    SolverInput::RawConstraint rc;
                    rc.indices.push_back((int)w_idx);
                    rc.coefficients.push_back(1.0);
                    for (idx_t row : group_rows[g]) {
                        for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                            double coeff = saved_obj_coefficients[t][row];
                            if (per_inner_was_avg) {
                                coeff /= static_cast<double>(group_rows[g].size());
                            }
                            if (std::abs(coeff) < 1e-15) continue;
                            idx_t var_idx = pre_indexer.Get(saved_obj_var_indices[t], row);
                            rc.indices.push_back((int)var_idx);
                            rc.coefficients.push_back(-coeff);
                        }
                    }
                    idx_t u_idx = first_u + g;
                    if (outer_is_min) {
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(-M_outer);
                        rc.sense = '>';
                        rc.rhs = -M_outer;
                    } else {
                        rc.indices.push_back((int)u_idx);
                        rc.coefficients.push_back(M_outer);
                        rc.sense = '<';
                        rc.rhs = M_outer;
                    }
                    solver_input.global_constraints.push_back(std::move(rc));
                }
                SolverInput::RawConstraint sum_u;
                for (idx_t g = 0; g < K; g++) {
                    sum_u.indices.push_back((int)(first_u + g));
                    sum_u.coefficients.push_back(1.0);
                }
                sum_u.sense = '>';
                sum_u.rhs = 1.0;
                solver_input.global_constraints.push_back(std::move(sum_u));
            }
        }
    } else if (flat_objective_agg != ObjectiveAggregateType::NONE && !saved_obj_var_indices.empty()) {
        // ================================================================
        // PATH A: Non-PER flat MIN/MAX objective (existing behavior)
        // ================================================================
        bool is_min_agg = (flat_objective_agg == ObjectiveAggregateType::MIN_AGG);
        bool is_easy = flat_objective_is_easy;

        idx_t z_idx = pre_indexer.global_block_start + solver_input.num_global_vars;

        // Create global variable z (continuous, unbounded)
        solver_input.num_global_vars += 1;
        solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
        solver_input.global_lower_bounds.push_back(-1e30);
        solver_input.global_upper_bounds.push_back(1e30);
        solver_input.global_obj_coeffs.push_back(1.0); // objective = z

        // Clear per-row objective (z is the sole objective term now)
        solver_input.objective_coefficients.clear();
        solver_input.objective_variable_indices.clear();

        if (is_easy) {
            char sense_char = is_min_agg ? '<' : '>';
            for (idx_t row = 0; row < num_rows; row++) {
                SolverInput::RawConstraint rc;
                rc.sense = sense_char;
                rc.rhs = 0.0;
                rc.indices.push_back((int)z_idx);
                rc.coefficients.push_back(1.0);
                for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                    idx_t var = saved_obj_var_indices[t];
                    double coeff = saved_obj_coefficients[t][row];
                    if (std::abs(coeff) < 1e-15) continue;
                    idx_t var_idx = pre_indexer.Get(var, row);
                    rc.indices.push_back((int)var_idx);
                    rc.coefficients.push_back(-coeff);
                }
                solver_input.global_constraints.push_back(std::move(rc));
            }
        } else {
            double M = compute_big_m();

            idx_t first_y_idx = z_idx + 1;
            solver_input.num_global_vars += num_rows;
            for (idx_t r = 0; r < num_rows; r++) {
                solver_input.global_variable_types.push_back(LogicalType::BOOLEAN);
                solver_input.global_lower_bounds.push_back(0.0);
                solver_input.global_upper_bounds.push_back(1.0);
                solver_input.global_obj_coeffs.push_back(0.0);
            }

            for (idx_t row = 0; row < num_rows; row++) {
                SolverInput::RawConstraint rc;
                rc.indices.push_back((int)z_idx);
                rc.coefficients.push_back(1.0);
                for (idx_t t = 0; t < saved_obj_var_indices.size(); t++) {
                    idx_t var = saved_obj_var_indices[t];
                    double coeff = saved_obj_coefficients[t][row];
                    if (std::abs(coeff) < 1e-15) continue;
                    idx_t var_idx = pre_indexer.Get(var, row);
                    rc.indices.push_back((int)var_idx);
                    rc.coefficients.push_back(-coeff);
                }
                idx_t y_idx = first_y_idx + row;
                if (is_min_agg) {
                    rc.indices.push_back((int)y_idx);
                    rc.coefficients.push_back(-M);
                    rc.sense = '>';
                    rc.rhs = -M;
                } else {
                    rc.indices.push_back((int)y_idx);
                    rc.coefficients.push_back(M);
                    rc.sense = '<';
                    rc.rhs = M;
                }
                solver_input.global_constraints.push_back(std::move(rc));
            }

            SolverInput::RawConstraint sum_y;
            for (idx_t row = 0; row < num_rows; row++) {
                sum_y.indices.push_back((int)(first_y_idx + row));
                sum_y.coefficients.push_back(1.0);
            }
            sum_y.sense = '>';
            sum_y.rhs = 1.0;
            solver_input.global_constraints.push_back(std::move(sum_y));
        }
    }

    // Build VarIndexer for solution readback (also used by model builder)
    gstate.var_indexer = VarIndexer::Build(solver_input);

    // Capture model size before solve (solver may move data)
    size_t bench_total_vars = gstate.var_indexer.total_vars;
    size_t bench_total_constraints = solver_input.constraints.size() + solver_input.global_constraints.size();

    if (bench) {
        model_timer.End();
        solver_timer.Start();
    }

    gstate.ilp_solution = SolveModel(solver_input);

    if (bench) {
        solver_timer.End();
        fprintf(stderr, "PACKDB_BENCH: model_construction_ms=%.2f\n", model_timer.Elapsed() * 1000.0);
        fprintf(stderr, "PACKDB_BENCH: solver_ms=%.2f\n", solver_timer.Elapsed() * 1000.0);
        fprintf(stderr, "PACKDB_BENCH: total_variables=%zu\n", bench_total_vars);
        fprintf(stderr, "PACKDB_BENCH: total_constraints=%zu\n", bench_total_constraints);
        fprintf(stderr, "PACKDB_BENCH: num_rows=%zu\n", (size_t)num_rows);
    }

    return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source (Producing Output)
//===--------------------------------------------------------------------===//
class DecideGlobalSourceState : public GlobalSourceState {
public:
    explicit DecideGlobalSourceState(const PhysicalDecide &op, DecideGlobalSinkState &sink) {
        sink.data.InitializeScan(scan_state);
        current_row_offset = 0;
    }

    ColumnDataScanState scan_state;
    idx_t current_row_offset; // Track which row we're at in the solution vector

    idx_t MaxThreads() override {
        return 1; // For simplicity, we'll make the source single-threaded.
    }
};

unique_ptr<GlobalSourceState> PhysicalDecide::GetGlobalSourceState(ClientContext &context) const {
    auto &sink = sink_state->Cast<DecideGlobalSinkState>();
    return make_uniq_base<GlobalSourceState, DecideGlobalSourceState>(*this, sink);
}

SourceResultType PhysicalDecide::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
    auto &gstate = sink_state->Cast<DecideGlobalSinkState>();
    auto &source_state = input.global_state.Cast<DecideGlobalSourceState>();

    // Scan the original buffered data
    gstate.data.Scan(source_state.scan_state, chunk);
    if (chunk.size() == 0) {
        return SourceResultType::FINISHED;
    }

    // All DECIDE vars (user + auxiliary) are in the output; projection above prunes aux vars
    idx_t total_decide_vars = decide_variables.size();
    idx_t chunk_size = chunk.size();

    // Fill in ALL DECIDE variable columns with solution values from ILP solver
    for (idx_t decide_var_idx = 0; decide_var_idx < total_decide_vars; decide_var_idx++) {
        // The DECIDE columns are appended at the end of the output
        idx_t column_idx = types.size() - total_decide_vars + decide_var_idx;

        auto &output_vector = chunk.data[column_idx];

        // Set vector to flat (each row has its own value)
        output_vector.SetVectorType(VectorType::FLAT_VECTOR);

        // Get the logical type for this DECIDE variable
        auto &decide_var = decide_variables[decide_var_idx]->Cast<BoundColumnRefExpression>();
        auto var_type = decide_var.return_type;

        // Get data pointer once based on type
        if (var_type == LogicalType::INTEGER || var_type == LogicalType::BIGINT) {
            // Use int32_t for INTEGER, int64_t for BIGINT
            if (var_type == LogicalType::INTEGER) {
                auto output_data = FlatVector::GetData<int32_t>(output_vector);

                for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                    idx_t global_row = source_state.current_row_offset + row_in_chunk;
                    idx_t solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row);

                    double solution_value = 0.0;
                    if (solution_idx < gstate.ilp_solution.size()) {
                        solution_value = gstate.ilp_solution[solution_idx];
                    }
                    int32_t int_value = static_cast<int32_t>(std::round(solution_value));
                    output_data[row_in_chunk] = int_value;
                }
            } else { // BIGINT
                auto output_data = FlatVector::GetData<int64_t>(output_vector);

                for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                    idx_t global_row = source_state.current_row_offset + row_in_chunk;
                    idx_t solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row);

                    double solution_value = 0.0;
                    if (solution_idx < gstate.ilp_solution.size()) {
                        solution_value = gstate.ilp_solution[solution_idx];
                    }
                    int64_t int_value = static_cast<int64_t>(std::round(solution_value));
                    output_data[row_in_chunk] = int_value;
                }
            }

        } else if (var_type == LogicalType::BOOLEAN) {
            auto output_data = FlatVector::GetData<bool>(output_vector);

            for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                idx_t global_row = source_state.current_row_offset + row_in_chunk;
                idx_t solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row);

                double solution_value = 0.0;
                if (solution_idx < gstate.ilp_solution.size()) {
                    solution_value = gstate.ilp_solution[solution_idx];
                }
                output_data[row_in_chunk] = (solution_value >= 0.5);
            }

        } else if (var_type == LogicalType::DOUBLE) {
            auto output_data = FlatVector::GetData<double>(output_vector);

            for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                idx_t global_row = source_state.current_row_offset + row_in_chunk;
                idx_t solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row);

                double solution_value = 0.0;
                if (solution_idx < gstate.ilp_solution.size()) {
                    solution_value = gstate.ilp_solution[solution_idx];
                }
                output_data[row_in_chunk] = solution_value;
            }

        } else {
            // Default to INTEGER
            auto output_data = FlatVector::GetData<int64_t>(output_vector);

            for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                idx_t global_row = source_state.current_row_offset + row_in_chunk;
                idx_t solution_idx = gstate.var_indexer.Get(decide_var_idx, global_row);

                double solution_value = 0.0;
                if (solution_idx < gstate.ilp_solution.size()) {
                    solution_value = gstate.ilp_solution[solution_idx];
                }
                output_data[row_in_chunk] = static_cast<int64_t>(std::round(solution_value));
            }
        }
    }

    // Update row offset for next chunk
    source_state.current_row_offset += chunk_size;

    return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
