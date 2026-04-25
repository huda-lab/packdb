#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include <cmath>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <unordered_set>
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
#include "duckdb/planner/expression/bound_between_expression.hpp"
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

//! Vectorized extraction of a chunk-result column into a vector<double>, multiplied by `sign`.
//! Throws InvalidInputException on NULL or non-finite values, citing `err_context`.
//! Fast path for DOUBLE; otherwise casts via VectorOperations::DefaultCast.
static void ExtractDoubleColumn(Vector &result_vec, idx_t count, double sign,
                                vector<double> &out, const char *err_context) {
	if (count == 0) {
		return;
	}
	UnifiedVectorFormat format;
	Vector cast_tmp(LogicalType::DOUBLE);
	Vector *src;
	if (result_vec.GetType().id() == LogicalTypeId::DOUBLE) {
		src = &result_vec;
	} else {
		VectorOperations::DefaultCast(result_vec, cast_tmp, count, false);
		src = &cast_tmp;
	}
	src->ToUnifiedFormat(count, format);
	auto data = UnifiedVectorFormat::GetData<double>(format);
	for (idx_t i = 0; i < count; i++) {
		idx_t idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			throw InvalidInputException(
				"DECIDE %s returned NULL at row %llu. "
				"NULL values are not allowed in optimization expressions. "
				"Use COALESCE() to handle NULLs or filter them with WHERE clause.",
				err_context, out.size());
		}
		double dv = data[idx];
		if (!std::isfinite(dv)) {
			throw InvalidInputException(
				"DECIDE %s contains invalid value (NaN or Infinity) at row %llu. "
				"Common causes:\n"
				"  • Division by zero in the expression\n"
				"  • Arithmetic overflow in calculations\n"
				"  • NULL values that propagated through math operations\n"
				"Check your expressions and input data.",
				err_context, out.size());
		}
		out.push_back(dv * sign);
	}
}

//! Compare two key tuples for grouping equality. Two NULLs in the same column
//! are treated as identical (entity grouping convention). PER call sites screen
//! NULLs out before reaching equality, so the choice does not affect them.
static bool KeyTuplesEqual(const vector<vector<Value>> &cols, idx_t row_a, idx_t row_b) {
	for (idx_t c = 0; c < cols.size(); c++) {
		auto &av = cols[c][row_a];
		auto &bv = cols[c][row_b];
		if (av.IsNull() != bv.IsNull()) {
			return false;
		}
		if (av.IsNull()) {
			continue;
		}
		if (!(av == bv)) {
			return false;
		}
	}
	return true;
}

//! Vectorized typed-hash grouping. Replaces the prior Value::ToString()-based
//! composite-string keying in entity mapping and PER grouping. Hashes are
//! computed per chunk via VectorOperations::Hash / CombineHash; collision
//! fallback uses per-row Values stored as `key_columns`.
//!
//!   key_exprs       — pre-transformed expressions for chunk evaluation, one per key column
//!   row_filter      — return true to include row in grouping (empty function = include all)
//!   null_excludes   — true = any NULL in a key column maps the row to INVALID_INDEX (PER semantics);
//!                     false = NULL is part of the composite key (entity semantics)
//!   out_row_to_group — sized num_rows; INVALID_INDEX for excluded rows, [0..K) otherwise
//!   out_num_groups   — K, count of distinct groups in input order
static void BuildGroupIds(const vector<unique_ptr<Expression>> &key_exprs,
                          ClientContext &context,
                          ColumnDataCollection &data,
                          idx_t num_rows,
                          const std::function<bool(idx_t)> &row_filter,
                          bool null_excludes,
                          vector<idx_t> &out_row_to_group,
                          idx_t &out_num_groups) {
	out_row_to_group.assign(num_rows, DConstants::INVALID_INDEX);
	out_num_groups = 0;
	if (num_rows == 0 || key_exprs.empty()) {
		return;
	}

	const idx_t num_key_cols = key_exprs.size();

	// Build one executor for all key columns; produces a multi-column result chunk per scan.
	ExpressionExecutor key_executor(context);
	vector<LogicalType> key_types;
	key_types.reserve(num_key_cols);
	for (auto &e : key_exprs) {
		key_executor.AddExpression(*e);
		key_types.push_back(e->return_type);
	}

	vector<hash_t> row_hashes;
	row_hashes.reserve(num_rows);
	vector<vector<Value>> key_columns(num_key_cols);
	for (idx_t c = 0; c < num_key_cols; c++) {
		key_columns[c].reserve(num_rows);
	}

	ColumnDataScanState scan;
	data.InitializeScan(scan);
	DataChunk chunk;
	chunk.Initialize(context, data.Types());
	DataChunk key_results;
	key_results.Initialize(context, key_types);
	Vector chunk_hashes(LogicalType::HASH);
	while (data.Scan(scan, chunk)) {
		idx_t count = chunk.size();
		if (count == 0) {
			continue;
		}
		key_results.Reset();
		key_executor.Execute(chunk, key_results);

		VectorOperations::Hash(key_results.data[0], chunk_hashes, count);
		for (idx_t c = 1; c < num_key_cols; c++) {
			VectorOperations::CombineHash(chunk_hashes, key_results.data[c], count);
		}
		auto hashes_data = FlatVector::GetData<hash_t>(chunk_hashes);
		for (idx_t i = 0; i < count; i++) {
			row_hashes.push_back(hashes_data[i]);
		}

		// Capture Values for equality fallback. (Skips Value::ToString() entirely.)
		for (idx_t c = 0; c < num_key_cols; c++) {
			auto &vec = key_results.data[c];
			for (idx_t i = 0; i < count; i++) {
				key_columns[c].push_back(vec.GetValue(i));
			}
		}
	}

	if (row_hashes.size() != num_rows) {
		throw InternalException(
			"DECIDE BuildGroupIds: chunk scan produced %llu rows, expected %llu",
			row_hashes.size(), num_rows);
	}

	std::unordered_multimap<hash_t, idx_t> hash_to_rep_row;
	hash_to_rep_row.reserve(num_rows);
	idx_t next_group = 0;
	for (idx_t row = 0; row < num_rows; row++) {
		if (row_filter && !row_filter(row)) {
			continue;
		}
		if (null_excludes) {
			bool has_null = false;
			for (idx_t c = 0; c < num_key_cols; c++) {
				if (key_columns[c][row].IsNull()) {
					has_null = true;
					break;
				}
			}
			if (has_null) {
				continue;
			}
		}
		hash_t h = row_hashes[row];
		auto range = hash_to_rep_row.equal_range(h);
		bool matched = false;
		for (auto it = range.first; it != range.second; ++it) {
			if (KeyTuplesEqual(key_columns, row, it->second)) {
				out_row_to_group[row] = out_row_to_group[it->second];
				matched = true;
				break;
			}
		}
		if (!matched) {
			out_row_to_group[row] = next_group++;
			hash_to_rep_row.emplace(h, row);
		}
	}
	out_num_groups = next_group;
}

struct NormalizedProductTerm {
	const BoundFunctionExpression *mul_func = nullptr;
	vector<const Expression *> coefficient_factors;
	vector<idx_t> decide_factors;
};

static const Expression *UnwrapBoundCasts(const Expression &expr) {
	const Expression *cur = &expr;
	while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		cur = cur->Cast<BoundCastExpression>().child.get();
	}
	return cur;
}

static bool IsBoundMultiply(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	return func.function.name == "*";
}

static void CollectMultiplicativeFactors(const Expression &expr, vector<const Expression *> &factors) {
	const Expression *cur = UnwrapBoundCasts(expr);
	if (IsBoundMultiply(*cur)) {
		auto &func = cur->Cast<BoundFunctionExpression>();
		for (auto &child : func.children) {
			CollectMultiplicativeFactors(*child, factors);
		}
		return;
	}
	factors.push_back(cur);
}

static unique_ptr<Expression> BuildCoefficientFromFactors(const vector<const Expression *> &factors,
                                                          const BoundFunctionExpression &mul_func) {
	if (factors.empty()) {
		return nullptr;
	}
	if (factors.size() == 1) {
		return factors[0]->Copy();
	}

	auto result = factors[0]->Copy();
	for (idx_t i = 1; i < factors.size(); i++) {
		vector<unique_ptr<Expression>> mul_children;
		mul_children.push_back(std::move(result));
		mul_children.push_back(factors[i]->Copy());
		unique_ptr<FunctionData> bind_info;
		if (mul_func.bind_info) {
			bind_info = mul_func.bind_info->Copy();
		}
		result = make_uniq_base<Expression, BoundFunctionExpression>(
		    mul_func.return_type, mul_func.function, std::move(mul_children), std::move(bind_info));
	}
	return result;
}

static bool TryGetBareDecideFactor(const Expression &expr, const PhysicalDecide &op, idx_t &var_idx) {
	const Expression *cur = UnwrapBoundCasts(expr);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	var_idx = op.FindDecideVariable(*cur);
	return var_idx != DConstants::INVALID_INDEX;
}

static bool ClassifyNormalizedProduct(const Expression &expr, const PhysicalDecide &op,
                                      NormalizedProductTerm &result) {
	const Expression *root = UnwrapBoundCasts(expr);
	if (!IsBoundMultiply(*root)) {
		return false;
	}

	result = NormalizedProductTerm();
	result.mul_func = &root->Cast<BoundFunctionExpression>();

	vector<const Expression *> factors;
	CollectMultiplicativeFactors(*root, factors);
	for (auto *factor : factors) {
		idx_t var_idx = DConstants::INVALID_INDEX;
		if (TryGetBareDecideFactor(*factor, op, var_idx)) {
			result.decide_factors.push_back(var_idx);
			continue;
		}
		if (op.FindDecideVariable(*factor) != DConstants::INVALID_INDEX) {
			throw InvalidInputException(
			    "DECIDE expression contains an unsupported product factor that still "
			    "references decision variables after normalization (total degree > 2 "
			    "or unexpanded nonlinear product). Products must be data factors times "
			    "one DECIDE variable, or data factors times two different DECIDE variables.");
		}
		result.coefficient_factors.push_back(factor);
	}

	if (result.decide_factors.size() > 2) {
		throw InvalidInputException(
		    "DECIDE expression contains a product of decision variables with total degree > 2. "
		    "Only linear products and bilinear products of two different DECIDE variables are supported.");
	}
	if (result.decide_factors.size() == 2 && result.decide_factors[0] == result.decide_factors[1]) {
		throw InvalidInputException(
		    "DECIDE expression contains a same-variable product that is not in a supported "
		    "quadratic form. Use POWER(linear_expr, 2) or (linear_expr) * (linear_expr) "
		    "for quadratic terms.");
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Expression Analysis Helper Functions
//===--------------------------------------------------------------------===//

// ExpressionIterator::EnumerateChildren has no const overload; this wrapper
// isolates the const_cast so no call site needs to mention it.
static void EnumerateChildrenConst(const Expression &expr,
                                   const std::function<void(unique_ptr<Expression> &)> &callback) {
	ExpressionIterator::EnumerateChildren(const_cast<Expression &>(expr), callback);
}

idx_t PhysicalDecide::FindDecideVariable(const Expression &expr) const {
    // Base case: check if this is a column reference to a DECIDE variable
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        auto it = decide_variable_map.find(colref.binding);
        if (it != decide_variable_map.end()) {
            return it->second;
        }
    }

    // Recursive case: search in children
    idx_t result = DConstants::INVALID_INDEX;
    EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
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
    EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
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

        // Division is linear iff the divisor is decide-var-free and the
        // numerator is linear. `x / 2` is a coefficient scale (linear);
        // `x / y` is non-linear (already rejected upstream by the bind-time
        // validator, but we guard here anyway for defence-in-depth).
        if (fname == "/" && func.children.size() == 2) {
            if (FindDecideVariable(*func.children[1]) != DConstants::INVALID_INDEX) {
                return false;
            }
            return IsLinearInDecideVars(*func.children[0]);
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
                                                     std::move(filtered_children),
                                                     func.bind_info ? func.bind_info->Copy() : nullptr);
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
        Expression::Equals(*func.children[0], *func.children[1]) &&
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

        // Unary minus: recurse and flip sign of every produced term.
        if (func.function.name == "-" && func.children.size() == 1) {
            idx_t before = out_terms.size();
            ExtractTerms(*func.children[0], out_terms);
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

        // Division by a DECIDE-variable-free expression: recurse into the
        // numerator and wrap every produced term's coefficient in `coef / divisor`.
        // Division where the divisor itself contains a decide variable is
        // non-linear and is already rejected upstream by the bind-time validator.
        // Cast both sides to the `/` function's expected argument types so
        // an extracted integer coefficient doesn't silently turn into
        // integer-division truncation (e.g., `x/2` gave 0 when coef was INT 1).
        if (func.function.name == "/" && func.children.size() == 2 &&
            FindDecideVariable(*func.children[1]) == DConstants::INVALID_INDEX) {
            idx_t before = out_terms.size();
            ExtractTerms(*func.children[0], out_terms);
            D_ASSERT(func.function.arguments.size() == 2);
            const auto &num_type = func.function.arguments[0];
            const auto &denom_type = func.function.arguments[1];
            for (idx_t i = before; i < out_terms.size(); i++) {
                auto coef = BoundCastExpression::AddDefaultCastToType(
                    std::move(out_terms[i].coefficient), num_type);
                auto divisor = BoundCastExpression::AddDefaultCastToType(
                    func.children[1]->Copy(), denom_type);
                vector<unique_ptr<Expression>> div_children;
                div_children.push_back(std::move(coef));
                div_children.push_back(std::move(divisor));
                out_terms[i].coefficient = make_uniq_base<Expression, BoundFunctionExpression>(
                    func.return_type, func.function, std::move(div_children), nullptr);
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
    EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
        if (!found && child && BoundExpressionContainsAggregate(*child)) {
            found = true;
        }
    });
    return found;
}

// PackDB: reject an aggregate whose effective row set is empty (after WHEN
// filtering). An empty aggregate has no well-defined value — MIN(∅)=+∞ and
// MAX(∅)=-∞ cannot be represented in the MILP encoding, SUM(∅)=0 and AVG(∅)
// is undefined. Without this guard the hard-direction MIN/MAX z_k auxiliary
// floats free and silently vacates the outer constraint or objective.
static void RejectEmptyAggregate(idx_t effective_row_count, const char *what, const char *ctx) {
    if (effective_row_count == 0) {
        throw InvalidInputException(
            "DECIDE empty row set for %s in %s. "
            "An empty aggregate has no well-defined value; check your WHEN clause.",
            what, ctx);
    }
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
    for (idx_t i = 0; i < this->decide_variables.size(); i++) {
        auto &var = this->decide_variables[i]->Cast<BoundColumnRefExpression>();
        decide_variable_map[var.binding] = i;
    }
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
			string per_suffix = " PER ";
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
        // Pre-absorb simple variable bounds (x OP const / BETWEEN) into column-bound
        // arrays so AnalyzeConstraint can skip emitting one DecideConstraint per row
        // for constraints that are fully captured by column bounds.
        idx_t num_decide_vars = op.decide_variables.size();
        absorbed_lower_bounds.assign(num_decide_vars, 0.0);
        absorbed_upper_bounds.assign(num_decide_vars, 1e30);
        for (idx_t var = 0; var < num_decide_vars; var++) {
            auto &decide_var = op.decide_variables[var]->Cast<BoundColumnRefExpression>();
            if (decide_var.return_type == LogicalType::BOOLEAN) {
                absorbed_upper_bounds[var] = 1.0;
            }
        }
        if (op.decide_constraints) {
            TraverseBoundsConstraints(*op.decide_constraints, absorbed_lower_bounds,
                                      absorbed_upper_bounds);
        }

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
                           vector<unique_ptr<Expression>> per_columns = {}) {
        auto &expr = *expr_ptr;
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                // PackDB: PER wrapper — outermost layer
                if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
                    // child[0] = the constraint (possibly WHEN-wrapped)
                    // children[1..N] = the PER column expressions
                    vector<unique_ptr<Expression>> per_cols;
                    for (idx_t i = 1; i < conj.children.size(); i++) {
                        per_cols.push_back(conj.children[i]->Copy());
                    }
                    AnalyzeConstraint(conj.children[0], std::move(when_condition),
                                      std::move(per_cols));
                    break;
                }
                // PackDB: Check if this is a WHEN constraint wrapper
                if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
                    // child[0] = the actual constraint, child[1] = the WHEN condition
                    AnalyzeConstraint(conj.children[0], conj.children[1]->Copy(),
                                      std::move(per_columns));
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

                // Skip comparisons whose entire semantics are already captured in
                // column bounds by the constructor's absorption pass. Emitting a
                // DecideConstraint here would add num_rows redundant model rows.
                // WHEN-wrapped comparisons are never absorbed (see
                // TraverseBoundsConstraints WHEN_CONSTRAINT_TAG branch), so
                // skipping here is safe.
                if (absorbed_bound_exprs.count(&expr)) {
                    break;
                }

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

                // Parse ABS MAXIMIZE upper-bound tag: marks a lower-bound ABS constraint
                // (aux >= inner or aux >= -inner) that needs Big-M upper bounds at finalization.
                if (comp.alias.size() > strlen(ABS_UB_POS_TAG_PREFIX) + 2 &&
                    comp.alias.substr(0, strlen(ABS_UB_POS_TAG_PREFIX)) == ABS_UB_POS_TAG_PREFIX) {
                    auto payload = comp.alias.substr(strlen(ABS_UB_POS_TAG_PREFIX));
                    constraint->abs_y_idx = std::stoull(payload.substr(0, payload.size() - 2));
                    constraint->abs_is_pos_bound = true;
                } else if (comp.alias.size() > strlen(ABS_UB_NEG_TAG_PREFIX) + 2 &&
                           comp.alias.substr(0, strlen(ABS_UB_NEG_TAG_PREFIX)) == ABS_UB_NEG_TAG_PREFIX) {
                    auto payload = comp.alias.substr(strlen(ABS_UB_NEG_TAG_PREFIX));
                    constraint->abs_y_idx = std::stoull(payload.substr(0, payload.size() - 2));
                    constraint->abs_is_pos_bound = false;
                }

                // Detect easy-direction MIN/MAX optimizer rewrite (see decide.hpp).
                if (comp.alias == MINMAX_EASY_REWRITE_TAG) {
                    constraint->was_minmax_easy = true;
                }

                // PackDB: Store WHEN condition and PER columns if present
                if (when_condition) {
                    constraint->when_condition = std::move(when_condition);
                }
                if (!per_columns.empty()) {
                    constraint->per_columns = std::move(per_columns);
                }

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

            // Multiplication: the parsed normalizer is responsible for algebraic
            // expansion; at physical planning we only flatten already-normalized
            // product factors for classification.
            if (fname == "*") {
                NormalizedProductTerm product;
                if (ClassifyNormalizedProduct(func, op, product)) {
                    if (product.decide_factors.size() == 2) {
                        Objective::BilinearTerm bt;
                        bt.var_a = product.decide_factors[0];
                        bt.var_b = product.decide_factors[1];
                        bt.coefficient = BuildCoefficientFromFactors(product.coefficient_factors, *product.mul_func);
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
                    Expression::Equals(*qf.children[0], *qf.children[1]) &&
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
            if (fname == "*") {
                // Scaled quadratic: const * POWER(expr, 2) or POWER(expr, 2) * const
                if (func.children.size() == 2) {
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
                }
                NormalizedProductTerm product;
                if (ClassifyNormalizedProduct(func, op, product)) {
                    if (product.decide_factors.size() == 2) {
                        BilinearConstraintTerm bt;
                        bt.var_a = product.decide_factors[0];
                        bt.var_b = product.decide_factors[1];
                        bt.coefficient = BuildCoefficientFromFactors(product.coefficient_factors, *product.mul_func);
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

        // PackDB: Check for PER wrapper on objective (outermost layer)
        vector<unique_ptr<Expression>> per_cols;
        if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
            auto &conj = expr->Cast<BoundConjunctionExpression>();
            if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
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
        } else if (BoundExpressionContainsAggregate(*expr)) {
            objective = make_uniq<Objective>();
            ExtractAggregateObjectiveTerms(*expr, *objective, 1);
            objective->when_condition = std::move(when_cond);
            objective->per_columns = std::move(per_cols);
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

                            bool is_integer_var =
                                (op.decide_variables[var_idx]->return_type.id() == LogicalTypeId::INTEGER ||
                                 op.decide_variables[var_idx]->return_type.id() == LogicalTypeId::BIGINT);
                            bool absorbed = true;
                            // Apply bound based on comparison type
                            if (comp.type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
                                upper_bounds[var_idx] = std::min(upper_bounds[var_idx], bound_value);
                            } else if (comp.type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
                                lower_bounds[var_idx] = std::max(lower_bounds[var_idx], bound_value);
                            } else if (comp.type == ExpressionType::COMPARE_EQUAL) {
                                lower_bounds[var_idx] = bound_value;
                                upper_bounds[var_idx] = bound_value;
                            } else if (comp.type == ExpressionType::COMPARE_LESSTHAN && is_integer_var) {
                                // x < bound → x <= bound-1 for integers. REAL strict
                                // inequality has no valid absorption — leave it for
                                // the constraint path which rejects with a clear error.
                                upper_bounds[var_idx] = std::min(upper_bounds[var_idx], bound_value - 1.0);
                            } else if (comp.type == ExpressionType::COMPARE_GREATERTHAN && is_integer_var) {
                                // x > bound → x >= bound+1 for integers.
                                lower_bounds[var_idx] = std::max(lower_bounds[var_idx], bound_value + 1.0);
                            } else {
                                absorbed = false;
                            }
                            if (absorbed) {
                                absorbed_bound_exprs.insert(&expr);
                            }
                        }
                    }
                }
                break;
            }

            case ExpressionClass::BOUND_BETWEEN: {
                auto &between = expr.Cast<BoundBetweenExpression>();

                auto *input = between.input.get();
                while (input->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    input = input->Cast<BoundCastExpression>().child.get();
                }

                if (input->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    auto &colref = input->Cast<BoundColumnRefExpression>();
                    idx_t var_idx = DConstants::INVALID_INDEX;
                    for (idx_t i = 0; i < op.decide_variables.size(); i++) {
                        auto &decide_var = op.decide_variables[i]->Cast<BoundColumnRefExpression>();
                        if (colref.binding == decide_var.binding) {
                            var_idx = i;
                            break;
                        }
                    }

                    if (var_idx != DConstants::INVALID_INDEX) {
                        bool is_integer_var =
                            (op.decide_variables[var_idx]->return_type.id() == LogicalTypeId::INTEGER ||
                             op.decide_variables[var_idx]->return_type.id() == LogicalTypeId::BIGINT);

                        auto ExtractBound = [](const Expression *e) -> double {
                            while (e->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                                e = e->Cast<BoundCastExpression>().child.get();
                            }
                            if (e->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
                                return std::numeric_limits<double>::quiet_NaN();
                            }
                            auto &c = e->Cast<BoundConstantExpression>();
                            if (c.value.type().id() == LogicalTypeId::INTEGER ||
                                c.value.type().id() == LogicalTypeId::BIGINT) {
                                return static_cast<double>(c.value.GetValue<int64_t>());
                            }
                            return c.value.GetValue<double>();
                        };

                        double lo = ExtractBound(between.lower.get());
                        double hi = ExtractBound(between.upper.get());

                        if (!std::isnan(lo)) {
                            if (!between.lower_inclusive && is_integer_var) lo += 1.0;
                            lower_bounds[var_idx] = std::max(lower_bounds[var_idx], lo);
                        }
                        if (!std::isnan(hi)) {
                            if (!between.upper_inclusive && is_integer_var) hi -= 1.0;
                            upper_bounds[var_idx] = std::min(upper_bounds[var_idx], hi);
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

    //! Variable bounds populated once in the constructor from simple
    //! `x OP const` / `x BETWEEN a AND b` constraints. Finalize copies these
    //! into solver_input instead of re-walking the expression tree.
    vector<double> absorbed_lower_bounds;
    vector<double> absorbed_upper_bounds;
    //! BOUND_COMPARISON expression pointers that were fully absorbed into
    //! column bounds — AnalyzeConstraint skips these to avoid emitting
    //! `num_rows` redundant per-row model rows per absorbed bound.
    std::unordered_set<const Expression *> absorbed_bound_exprs;

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
    auto data_types = gstate.data.Types();
    for (idx_t scope_idx = 0; scope_idx < entity_scopes.size(); scope_idx++) {
        auto &scope = entity_scopes[scope_idx];
        EntityMapping mapping;

        // Build BoundReferenceExpression for each entity-key physical column index
        // so BuildGroupIds can run them through one ExpressionExecutor.
        vector<unique_ptr<Expression>> key_exprs;
        key_exprs.reserve(scope.entity_key_physical_indices.size());
        for (idx_t col_idx = 0; col_idx < scope.entity_key_physical_indices.size(); col_idx++) {
            idx_t phys_idx = scope.entity_key_physical_indices[col_idx];
            key_exprs.push_back(make_uniq_base<Expression, BoundReferenceExpression>(
                data_types[phys_idx], phys_idx));
        }

        BuildGroupIds(key_exprs, context, gstate.data, num_rows,
                      std::function<bool(idx_t)>{}, /*null_excludes=*/false,
                      mapping.row_to_entity, mapping.num_entities);
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

    // Evaluate N boolean filter expressions in a single scan over gstate.data.
    auto EvaluateBooleanMasks = [&](const vector<const Expression*> &conditions) -> vector<vector<bool>> {
        if (conditions.empty()) return {};

        vector<unique_ptr<Expression>> transformed;
        transformed.reserve(conditions.size());
        for (auto *cond : conditions) {
            transformed.push_back(TransformToChunkExpression(*cond, context));
        }

        ExpressionExecutor cond_executor(context);
        for (auto &expr : transformed) {
            cond_executor.AddExpression(*expr);
        }

        vector<vector<bool>> masks(conditions.size());
        for (auto &m : masks) m.reserve(num_rows);

        vector<LogicalType> result_types(conditions.size(), LogicalType::BOOLEAN);

        ColumnDataScanState cond_scan_state;
        gstate.data.InitializeScan(cond_scan_state);
        DataChunk cond_chunk;
        cond_chunk.Initialize(context, gstate.data.Types());

        while (gstate.data.Scan(cond_scan_state, cond_chunk)) {
            DataChunk cond_result;
            cond_result.Initialize(context, result_types);
            cond_executor.Execute(cond_chunk, cond_result);

            for (idx_t col = 0; col < conditions.size(); col++) {
                auto &vec = cond_result.data[col];
                for (idx_t row = 0; row < cond_chunk.size(); row++) {
                    Value val = vec.GetValue(row);
                    masks[col].push_back(val.IsNull() ? false : val.GetValue<bool>());
                }
            }
        }
        return masks;
    };

    auto EvaluateBooleanMask = [&](const Expression &condition) -> vector<bool> {
        return EvaluateBooleanMasks({&condition})[0];
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
        eval_const.abs_y_idx = constraint->abs_y_idx;
        eval_const.abs_is_pos_bound = constraint->abs_is_pos_bound;

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

        if (constraint->lhs_is_aggregate) {
            // Collect all per-term filter expressions and their target states, then
            // batch-evaluate them in a single scan instead of one scan per filter.
            struct FilterSlot { const Expression *cond; TermFilterState *state; };
            vector<FilterSlot> filter_slots;

            for (idx_t i = 0; i < constraint->lhs_terms.size(); i++) {
                auto &term = constraint->lhs_terms[i];
                term_filters[i].avg_scale = term.avg_scale;
                if (term.filter) {
                    filter_slots.push_back({term.filter.get(), &term_filters[i]});
                } else {
                    has_unfiltered_aggregate_part = true;
                }
            }
            for (idx_t i = 0; i < constraint->bilinear_terms.size(); i++) {
                auto &term = constraint->bilinear_terms[i];
                bilinear_filters[i].avg_scale = term.avg_scale;
                if (term.filter) {
                    filter_slots.push_back({term.filter.get(), &bilinear_filters[i]});
                } else {
                    has_unfiltered_aggregate_part = true;
                }
            }
            for (idx_t i = 0; i < constraint->quadratic_groups.size(); i++) {
                auto &group = constraint->quadratic_groups[i];
                quadratic_filters[i].avg_scale = group.avg_scale;
                if (group.filter) {
                    filter_slots.push_back({group.filter.get(), &quadratic_filters[i]});
                } else {
                    has_unfiltered_aggregate_part = true;
                }
            }

            if (!filter_slots.empty()) {
                vector<const Expression *> cond_ptrs;
                cond_ptrs.reserve(filter_slots.size());
                for (auto &s : filter_slots) cond_ptrs.push_back(s.cond);

                auto masks = EvaluateBooleanMasks(cond_ptrs);

                has_local_filters = true;
                for (idx_t i = 0; i < filter_slots.size(); i++) {
                    if (masks[i].size() != num_rows) {
                        throw InternalException(
                            "DECIDE aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                            num_rows, masks[i].size());
                    }
                    filter_slots[i].state->mask = std::move(masks[i]);
                    filter_slots[i].state->has_filter = true;
                    for (idx_t row = 0; row < num_rows; row++) {
                        if (filter_slots[i].state->mask[row]) {
                            local_row_active[row] = true;
                        }
                    }
                }
            }

            if (has_unfiltered_aggregate_part) {
                std::fill(local_row_active.begin(), local_row_active.end(), true);
            }
        }

        // Batch all linear coefficient expressions into one ExpressionExecutor and
        // produce a multi-column result chunk per scan iteration.
        vector<unique_ptr<Expression>> transformed_coefs;
        transformed_coefs.reserve(constraint->lhs_terms.size());
        vector<LogicalType> coef_result_types;
        coef_result_types.reserve(constraint->lhs_terms.size());
        ExpressionExecutor coef_executor(context);
        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            auto &term = constraint->lhs_terms[term_idx];
            transformed_coefs.push_back(TransformToChunkExpression(*term.coefficient, context));
            coef_result_types.push_back(transformed_coefs.back()->return_type);
            try {
                coef_executor.AddExpression(*transformed_coefs.back());
            } catch (const std::exception &e) {
                throw InternalException("Failed to add expression for term %llu: %s\nOriginal: %s\nTransformed: %s",
                    term_idx, e.what(), term.coefficient->ToString(), transformed_coefs.back()->ToString());
            }
            eval_const.row_coefficients[term_idx].reserve(num_rows);
        }

        DataChunk coef_results;
        if (!constraint->lhs_terms.empty()) {
            coef_results.Initialize(context, coef_result_types);
        }

        while (gstate.data.Scan(scan_state, chunk)) {
            if (constraint->lhs_terms.empty()) {
                continue;
            }
            coef_results.Reset();
            coef_executor.Execute(chunk, coef_results);
            for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
                ExtractDoubleColumn(coef_results.data[term_idx], chunk.size(),
                                    constraint->lhs_terms[term_idx].sign,
                                    eval_const.row_coefficients[term_idx],
                                    "constraint coefficient");
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

            DataChunk rhs_result;
            vector<LogicalType> result_types = {transformed_rhs->return_type};
            rhs_result.Initialize(context, result_types);
            while (gstate.data.Scan(rhs_scan_state, rhs_chunk)) {
                rhs_result.Reset();
                rhs_executor.Execute(rhs_chunk, rhs_result);
                ExtractDoubleColumn(rhs_result.data[0], rhs_chunk.size(), 1.0,
                                    eval_const.rhs_values,
                                    "constraint right-hand side");
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
            vector<bool> when_mask;
            if (has_when) {
                when_mask = EvaluateBooleanMask(*constraint->when_condition);
            }

            auto row_is_included = [&](idx_t row) {
                if (has_when && !when_mask[row]) {
                    return false;
                }
                if (has_local_filters && !local_row_active[row]) {
                    return false;
                }
                return true;
            };

            if (has_per) {
                vector<unique_ptr<Expression>> key_exprs;
                key_exprs.reserve(constraint->per_columns.size());
                for (auto &col : constraint->per_columns) {
                    key_exprs.push_back(TransformToChunkExpression(*col, context));
                }
                BuildGroupIds(key_exprs, context, gstate.data, num_rows,
                              row_is_included, /*null_excludes=*/true,
                              eval_const.row_group_ids, eval_const.num_groups);
                // PER: individual empty groups are skipped downstream (preserved),
                // but reject when *every* group is empty (the aggregate as a whole
                // sees no rows after WHEN filtering). Per-row constraints are
                // exempt — a per-row WHEN matching zero rows is a valid no-op
                // (the constraint applies to no rows), not an empty aggregate.
                // Easy-direction MIN/MAX were rewritten by the optimizer to
                // per-row form but still count as aggregates for rejection.
                if (constraint->lhs_is_aggregate || constraint->was_minmax_easy) {
                    RejectEmptyAggregate(eval_const.num_groups, "aggregate", "constraint");
                }
            } else {
                eval_const.row_group_ids.resize(num_rows);
                // WHEN and/or aggregate-local WHEN (no PER): one group (group 0) for matching rows
                idx_t included_rows = 0;
                for (idx_t row = 0; row < num_rows; row++) {
                    bool inc = row_is_included(row);
                    eval_const.row_group_ids[row] = inc ? 0 : DConstants::INVALID_INDEX;
                    if (inc) included_rows++;
                }
                eval_const.num_groups = 1;
                if (constraint->lhs_is_aggregate || constraint->was_minmax_easy) {
                    RejectEmptyAggregate(included_rows, "aggregate", "constraint");
                }
            }
        }

        // Per-term aggregate-local WHEN: reject any term whose own filter mask
        // matches zero rows. Without this the term contributes nothing to the
        // constraint (its coefficients are all zero-masked at line 1829-1840);
        // for a MIN/MAX term routed via the z_k pathway, that would leave z_k
        // unpinned and silently vacuous. This guards composed-like LHS shapes
        // that flow through the lhs_terms path.
        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            if (!term_filters[term_idx].has_filter) continue;
            idx_t cnt = 0;
            auto &mask = term_filters[term_idx].mask;
            for (bool m : mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, "aggregate term", "constraint");
        }
        for (idx_t term_idx = 0; term_idx < constraint->bilinear_terms.size(); term_idx++) {
            if (!bilinear_filters[term_idx].has_filter) continue;
            idx_t cnt = 0;
            auto &mask = bilinear_filters[term_idx].mask;
            for (bool m : mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, "bilinear aggregate term", "constraint");
        }
        for (idx_t group_idx = 0; group_idx < constraint->quadratic_groups.size(); group_idx++) {
            if (!quadratic_filters[group_idx].has_filter) continue;
            idx_t cnt = 0;
            auto &mask = quadratic_filters[group_idx].mask;
            for (bool m : mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, "quadratic aggregate term", "constraint");
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

        // AVG(x) <> K special case: dividing LHS coefficients by the AVG denominator
        // produces fractional coefficients, which the NE integer-step guard rejects.
        // For pure linear LHS where every term is AVG-scaled, hoist the denominator to
        // the RHS instead — keep LHS as SUM and multiply per-group RHS by group size
        // in the deferred NE expansion. Mixed AVG/non-AVG terms or bilinear/quadratic
        // LHS fall through to the existing path (which may still reject).
        bool ne_avg_hoist = false;
        if (constraint->ne_indicator_idx != DConstants::INVALID_INDEX &&
            constraint->lhs_is_aggregate && !constraint->has_bilinear && !constraint->has_quadratic &&
            !constraint->lhs_terms.empty()) {
            ne_avg_hoist = true;
            for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
                if (!term_filters[term_idx].avg_scale) {
                    ne_avg_hoist = false;
                    break;
                }
            }
        }
        if (ne_avg_hoist) {
            eval_const.ne_avg_rhs_scale = true;
            for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
                term_filters[term_idx].avg_scale = false;
            }
        }

        for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
            if (term_filters[term_idx].avg_scale) {
                ScaleAvgRowCoefficients(eval_const.row_coefficients[term_idx], term_filters[term_idx].has_filter,
                                        term_filters[term_idx].mask);
            }
        }

        // Evaluate bilinear terms in constraint (if any).
        // Batch terms with coefficient expressions into a single ExpressionExecutor.
        if (constraint->has_bilinear) {
            const idx_t num_bl = constraint->bilinear_terms.size();
            vector<EvaluatedConstraint::BilinearTerm> ebts(num_bl);
            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &bt = constraint->bilinear_terms[term_idx];
                ebts[term_idx].var_a = bt.var_a;
                ebts[term_idx].var_b = bt.var_b;
            }

            vector<unique_ptr<Expression>> bl_transformed;
            vector<idx_t> bl_route;       // result column index → bilinear_terms index
            vector<LogicalType> bl_types;
            ExpressionExecutor bl_executor(context);
            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &bt = constraint->bilinear_terms[term_idx];
                if (!bt.coefficient) {
                    ebts[term_idx].row_coefficients.assign(num_rows, static_cast<double>(bt.sign));
                    continue;
                }
                bl_transformed.push_back(TransformToChunkExpression(*bt.coefficient, context));
                bl_types.push_back(bl_transformed.back()->return_type);
                bl_executor.AddExpression(*bl_transformed.back());
                bl_route.push_back(term_idx);
                ebts[term_idx].row_coefficients.reserve(num_rows);
            }

            if (!bl_transformed.empty()) {
                ColumnDataScanState bl_scan;
                gstate.data.InitializeScan(bl_scan);
                DataChunk bl_chunk;
                bl_chunk.Initialize(context, gstate.data.Types());
                DataChunk bl_results;
                bl_results.Initialize(context, bl_types);
                while (gstate.data.Scan(bl_scan, bl_chunk)) {
                    bl_results.Reset();
                    bl_executor.Execute(bl_chunk, bl_results);
                    for (idx_t j = 0; j < bl_route.size(); j++) {
                        idx_t term_idx = bl_route[j];
                        ExtractDoubleColumn(bl_results.data[j], bl_chunk.size(),
                                            constraint->bilinear_terms[term_idx].sign,
                                            ebts[term_idx].row_coefficients,
                                            "bilinear constraint coefficient");
                    }
                }
            }

            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &ebt = ebts[term_idx];
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

        // Evaluate quadratic groups in constraint (POWER(expr, 2) / self-products).
        // Per group, batch all inner_terms into a single ExpressionExecutor.
        if (constraint->has_quadratic) {
            eval_const.has_quadratic = true;
            for (idx_t group_idx = 0; group_idx < constraint->quadratic_groups.size(); group_idx++) {
                auto &qg = constraint->quadratic_groups[group_idx];
                EvaluatedConstraint::QuadraticGroup eqg;
                eqg.sign = qg.sign;

                vector<unique_ptr<Expression>> q_transformed;
                vector<LogicalType> q_types;
                ExpressionExecutor q_executor(context);
                eqg.row_coefficients.resize(qg.inner_terms.size());
                for (idx_t inner_idx = 0; inner_idx < qg.inner_terms.size(); inner_idx++) {
                    auto &term = qg.inner_terms[inner_idx];
                    eqg.variable_indices.push_back(term.variable_index);
                    q_transformed.push_back(TransformToChunkExpression(*term.coefficient, context));
                    q_types.push_back(q_transformed.back()->return_type);
                    q_executor.AddExpression(*q_transformed.back());
                    eqg.row_coefficients[inner_idx].reserve(num_rows);
                }

                if (!qg.inner_terms.empty()) {
                    ColumnDataScanState qscan;
                    gstate.data.InitializeScan(qscan);
                    DataChunk qchunk;
                    qchunk.Initialize(context, gstate.data.Types());
                    DataChunk q_results;
                    q_results.Initialize(context, q_types);
                    while (gstate.data.Scan(qscan, qchunk)) {
                        q_results.Reset();
                        q_executor.Execute(qchunk, q_results);
                        for (idx_t inner_idx = 0; inner_idx < qg.inner_terms.size(); inner_idx++) {
                            ExtractDoubleColumn(q_results.data[inner_idx], qchunk.size(),
                                                qg.inner_terms[inner_idx].sign,
                                                eqg.row_coefficients[inner_idx],
                                                "quadratic constraint coefficient");
                        }
                    }
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
            // Pre-size filters + out_coeffs, snapshot variable indices, and collect
            // all filter expressions so they can be batch-evaluated in one scan.
            struct ObjFilterSlot { const Expression *cond; TermFilterState *state; };
            vector<ObjFilterSlot> obj_filter_slots;

            for (auto &b : buckets) {
                b.out_term_filters->resize(b.src_terms->size());
                b.out_coeffs->resize(b.src_terms->size());
                for (idx_t term_idx = 0; term_idx < b.src_terms->size(); term_idx++) {
                    auto &term = (*b.src_terms)[term_idx];
                    (*b.out_term_filters)[term_idx].avg_scale = term.avg_scale;
                    if (term.filter) {
                        (*b.out_term_filters)[term_idx].has_filter = true;
                        obj_filter_slots.push_back({term.filter.get(), &(*b.out_term_filters)[term_idx]});
                    }
                    b.out_var_indices->push_back(term.variable_index);
                }
            }

            if (!obj_filter_slots.empty()) {
                vector<const Expression *> cond_ptrs;
                cond_ptrs.reserve(obj_filter_slots.size());
                for (auto &s : obj_filter_slots) cond_ptrs.push_back(s.cond);
                auto masks = EvaluateBooleanMasks(cond_ptrs);
                for (idx_t i = 0; i < obj_filter_slots.size(); i++) {
                    if (masks[i].size() != num_rows) {
                        throw InternalException(
                            "DECIDE objective aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                            num_rows, masks[i].size());
                    }
                    obj_filter_slots[i].state->mask = std::move(masks[i]);
                }
            }

            // Flatten all coefficient expressions across buckets. `route[i]` names
            // the bucket and the position within that bucket that flat term `i`
            // writes to — so the scan loop can route each evaluated value back
            // without knowing which bucket it came from.
            vector<unique_ptr<Expression>> flat_coeffs;
            vector<pair<idx_t, idx_t>> route; // (bucket_idx, term_idx)
            vector<LogicalType> obj_result_types;
            ExpressionExecutor obj_executor(context);
            for (idx_t b_idx = 0; b_idx < buckets.size(); b_idx++) {
                auto &b = buckets[b_idx];
                for (idx_t term_idx = 0; term_idx < b.src_terms->size(); term_idx++) {
                    flat_coeffs.push_back(
                        TransformToChunkExpression(*(*b.src_terms)[term_idx].coefficient, context));
                    obj_result_types.push_back(flat_coeffs.back()->return_type);
                    obj_executor.AddExpression(*flat_coeffs.back());
                    route.emplace_back(b_idx, term_idx);
                    (*b.out_coeffs)[term_idx].reserve(num_rows);
                }
            }

            ColumnDataScanState obj_scan_state;
            gstate.data.InitializeScan(obj_scan_state);
            DataChunk obj_chunk;
            obj_chunk.Initialize(context, gstate.data.Types());
            DataChunk obj_results;
            if (!flat_coeffs.empty()) {
                obj_results.Initialize(context, obj_result_types);
            }

            while (gstate.data.Scan(obj_scan_state, obj_chunk)) {
                if (flat_coeffs.empty()) {
                    continue;
                }
                obj_results.Reset();
                obj_executor.Execute(obj_chunk, obj_results);
                for (idx_t j = 0; j < flat_coeffs.size(); j++) {
                    auto &b = buckets[route[j].first];
                    idx_t term_idx = route[j].second;
                    auto &out = (*b.out_coeffs)[term_idx];
                    int term_sign = (*b.src_terms)[term_idx].sign;
                    ExtractDoubleColumn(obj_results.data[j], obj_chunk.size(),
                                        static_cast<double>(term_sign), out,
                                        "objective coefficient");
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

            for (auto &b : buckets) {
                for (idx_t term_idx = 0; term_idx < b.out_term_filters->size(); term_idx++) {
                    if (!(*b.out_term_filters)[term_idx].has_filter) continue;
                    idx_t cnt = 0;
                    auto &mask = (*b.out_term_filters)[term_idx].mask;
                    for (bool m : mask) if (m) cnt++;
                    RejectEmptyAggregate(cnt, "aggregate term", "objective");
                }
            }
        }

        // Reject an objective-level WHEN that matches zero rows. Hoisted out
        // of the `if (!buckets.empty())` block so it also covers bilinear-only
        // objectives (where `terms` is empty but a bilinear WHEN filter still
        // needs to be guarded). Without this, flat MIN/MAX objectives build a
        // z aux + per-row linking over all num_rows, with every linking
        // constraint vacuously satisfied — the solver drives z to whatever
        // extreme the objective sense prefers.
        if (objective_has_when) {
            idx_t cnt = 0;
            for (bool m : objective_when_mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, "aggregate", "objective");
        }

        obj_bilinear_filters.resize(gstate.objective->bilinear_terms.size());
        {
            struct BilFilterSlot { const Expression *cond; idx_t term_idx; };
            vector<BilFilterSlot> bil_slots;
            for (idx_t i = 0; i < gstate.objective->bilinear_terms.size(); i++) {
                auto &term = gstate.objective->bilinear_terms[i];
                obj_bilinear_filters[i].avg_scale = term.avg_scale;
                if (term.filter) {
                    obj_bilinear_filters[i].has_filter = true;
                    bil_slots.push_back({term.filter.get(), i});
                }
            }
            if (!bil_slots.empty()) {
                vector<const Expression *> cond_ptrs;
                cond_ptrs.reserve(bil_slots.size());
                for (auto &s : bil_slots) cond_ptrs.push_back(s.cond);
                auto masks = EvaluateBooleanMasks(cond_ptrs);
                for (idx_t i = 0; i < bil_slots.size(); i++) {
                    idx_t tidx = bil_slots[i].term_idx;
                    if (masks[i].size() != num_rows) {
                        throw InternalException(
                            "DECIDE objective aggregate-local WHEN mask size mismatch: expected %llu rows, got %llu",
                            num_rows, masks[i].size());
                    }
                    obj_bilinear_filters[tidx].mask = std::move(masks[i]);
                    idx_t cnt = 0;
                    for (bool m : obj_bilinear_filters[tidx].mask) if (m) cnt++;
                    RejectEmptyAggregate(cnt, "bilinear aggregate term", "objective");
                }
            }
        }

        // No extra debug here; solver output will show timings/objective

        // Evaluate bilinear term coefficients (non-Boolean pairs left by optimizer).
        // Batch terms with coefficient expressions into a single ExpressionExecutor.
        if (gstate.objective->has_bilinear) {
            const idx_t num_bl = gstate.objective->bilinear_terms.size();
            vector<DecideGlobalSinkState::EvaluatedBilinearTerm> ebts(num_bl);
            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &bt = gstate.objective->bilinear_terms[term_idx];
                ebts[term_idx].var_a = bt.var_a;
                ebts[term_idx].var_b = bt.var_b;
            }

            vector<unique_ptr<Expression>> bl_transformed;
            vector<idx_t> bl_route;
            vector<LogicalType> bl_types;
            ExpressionExecutor bl_executor(context);
            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &bt = gstate.objective->bilinear_terms[term_idx];
                if (!bt.coefficient) {
                    ebts[term_idx].row_coefficients.assign(num_rows, static_cast<double>(bt.sign));
                    continue;
                }
                bl_transformed.push_back(TransformToChunkExpression(*bt.coefficient, context));
                bl_types.push_back(bl_transformed.back()->return_type);
                bl_executor.AddExpression(*bl_transformed.back());
                bl_route.push_back(term_idx);
                ebts[term_idx].row_coefficients.reserve(num_rows);
            }

            if (!bl_transformed.empty()) {
                ColumnDataScanState bl_scan;
                gstate.data.InitializeScan(bl_scan);
                DataChunk bl_chunk;
                bl_chunk.Initialize(context, gstate.data.Types());
                DataChunk bl_results;
                bl_results.Initialize(context, bl_types);
                while (gstate.data.Scan(bl_scan, bl_chunk)) {
                    bl_results.Reset();
                    bl_executor.Execute(bl_chunk, bl_results);
                    for (idx_t j = 0; j < bl_route.size(); j++) {
                        idx_t term_idx = bl_route[j];
                        ExtractDoubleColumn(bl_results.data[j], bl_chunk.size(),
                                            gstate.objective->bilinear_terms[term_idx].sign,
                                            ebts[term_idx].row_coefficients,
                                            "bilinear objective coefficient");
                    }
                }
            }

            for (idx_t term_idx = 0; term_idx < num_bl; term_idx++) {
                auto &ebt = ebts[term_idx];
                if (obj_bilinear_filters[term_idx].has_filter) {
                    auto &mask = obj_bilinear_filters[term_idx].mask;
                    for (idx_t row = 0; row < ebt.row_coefficients.size(); row++) {
                        if (!mask[row]) {
                            ebt.row_coefficients[row] = 0.0;
                        }
                    }
                }
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
    for (idx_t var = 0; var < num_decide_vars; var++) {
        auto &decide_var = decide_variables[var]->Cast<BoundColumnRefExpression>();
        solver_input.variable_types[var] = decide_var.return_type;
    }

    // Bounds were absorbed in the gstate constructor from simple
    // `x OP const` / BETWEEN constraints; those comparisons were skipped in
    // AnalyzeConstraint so we don't re-emit them as per-row model rows.
    solver_input.lower_bounds = gstate.absorbed_lower_bounds;
    solver_input.upper_bounds = gstate.absorbed_upper_bounds;

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

    // Generate Big-M upper-bound constraints for MAXIMIZE + ABS auxiliary variables.
    // For each AbsMaximizeLink, find the two tagged lower-bound EvaluatedConstraints
    // (C1: aux >= inner tagged ABS_UB_POS, C2: aux >= -inner tagged ABS_UB_NEG) and emit:
    //   C_ub1: derived from C1, add y with coeff +2M, comparison <=, rhs[r] += 2M
    //   C_ub2: derived from C2, add y with coeff -2M, comparison <=, rhs unchanged
    // Together with C1/C2 these force aux = |inner| under MAXIMIZE.
    if (!abs_maximize_links.empty()) {
        struct AbsConstraintPair {
            idx_t c1 = DConstants::INVALID_INDEX;
            idx_t c2 = DConstants::INVALID_INDEX;
        };
        unordered_map<idx_t, AbsConstraintPair> abs_tag_map;
        for (idx_t ci = 0; ci < gstate.evaluated_constraints.size(); ci++) {
            auto &ec = gstate.evaluated_constraints[ci];
            if (ec.abs_y_idx == DConstants::INVALID_INDEX) {
                continue;
            }
            if (ec.abs_is_pos_bound) {
                abs_tag_map[ec.abs_y_idx].c1 = ci;
            } else {
                abs_tag_map[ec.abs_y_idx].c2 = ci;
            }
        }

        for (auto &link : abs_maximize_links) {
            auto it = abs_tag_map.find(link.y_idx);
            D_ASSERT(it != abs_tag_map.end() &&
                     it->second.c1 != DConstants::INVALID_INDEX &&
                     it->second.c2 != DConstants::INVALID_INDEX);

            // Copy all data out of C1 and C2 before any push_back that could reallocate
            // the evaluated_constraints vector and invalidate references.
            auto c1_vis   = gstate.evaluated_constraints[it->second.c1].variable_indices;
            auto c1_rcs   = gstate.evaluated_constraints[it->second.c1].row_coefficients;
            auto c1_rhs   = gstate.evaluated_constraints[it->second.c1].rhs_values;
            auto c1_rgids = gstate.evaluated_constraints[it->second.c1].row_group_ids;
            auto c1_ngrps = gstate.evaluated_constraints[it->second.c1].num_groups;

            auto c2_vis   = gstate.evaluated_constraints[it->second.c2].variable_indices;
            auto c2_rcs   = gstate.evaluated_constraints[it->second.c2].row_coefficients;
            auto c2_rhs   = gstate.evaluated_constraints[it->second.c2].rhs_values;
            auto c2_rgids = gstate.evaluated_constraints[it->second.c2].row_group_ids;
            auto c2_ngrps = gstate.evaluated_constraints[it->second.c2].num_groups;

            // Compute M = max over rows of |rhs[r]| + sum_{t: var != aux} |coeff[t][r]| * max(|lb|, |ub|).
            // This upper-bounds |inner| across all rows and variable values.
            double M = 0.0;
            for (idx_t r = 0; r < num_rows; r++) {
                double row_bound = std::abs(c1_rhs[r]);
                for (idx_t t = 0; t < c1_vis.size(); t++) {
                    if (c1_vis[t] == link.aux_idx) {
                        continue;
                    }
                    double lb = solver_input.lower_bounds[c1_vis[t]];
                    double ub = solver_input.upper_bounds[c1_vis[t]];
                    if (ub >= 1e20 || lb <= -1e20) {
                        throw InvalidInputException(
                            "MAXIMIZE SUM(ABS(...)) requires a finite bound on variable '%s'. "
                            "Add constraints '%s >= <lower>' and '%s <= <upper>'.",
                            decide_variables[c1_vis[t]]->Cast<BoundColumnRefExpression>().alias,
                            decide_variables[c1_vis[t]]->Cast<BoundColumnRefExpression>().alias,
                            decide_variables[c1_vis[t]]->Cast<BoundColumnRefExpression>().alias);
                    }
                    row_bound += std::abs(c1_rcs[t][r]) * std::max(std::abs(lb), std::abs(ub));
                }
                M = std::max(M, row_bound);
            }
            double two_M = 2.0 * M;

            // C_ub1: same as C1 but add y_idx with coeff +2M, flip to <=, rhs[r] += 2M
            EvaluatedConstraint ec_ub1;
            ec_ub1.variable_indices = c1_vis;
            ec_ub1.row_coefficients = c1_rcs;
            ec_ub1.variable_indices.push_back(link.y_idx);
            ec_ub1.row_coefficients.push_back(vector<double>(num_rows, two_M));
            ec_ub1.rhs_values.resize(num_rows);
            for (idx_t r = 0; r < num_rows; r++) {
                ec_ub1.rhs_values[r] = c1_rhs[r] + two_M;
            }
            ec_ub1.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
            ec_ub1.lhs_is_aggregate = false;
            ec_ub1.row_group_ids = c1_rgids;
            ec_ub1.num_groups = c1_ngrps;
            gstate.evaluated_constraints.push_back(std::move(ec_ub1));

            // C_ub2: same as C2 but add y_idx with coeff -2M, flip to <=, rhs unchanged
            EvaluatedConstraint ec_ub2;
            ec_ub2.variable_indices = c2_vis;
            ec_ub2.row_coefficients = c2_rcs;
            ec_ub2.variable_indices.push_back(link.y_idx);
            ec_ub2.row_coefficients.push_back(vector<double>(num_rows, -two_M));
            ec_ub2.rhs_values = c2_rhs;
            ec_ub2.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
            ec_ub2.lhs_is_aggregate = false;
            ec_ub2.row_group_ids = c2_rgids;
            ec_ub2.num_groups = c2_ngrps;
            gstate.evaluated_constraints.push_back(std::move(ec_ub2));
        }
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

        auto obj_row_is_included = [&](idx_t row) -> bool {
            if (objective_has_when && !objective_when_mask[row]) return false;
            if (!objective_row_has_local_term(row)) return false;
            return true;
        };

        vector<unique_ptr<Expression>> key_exprs;
        key_exprs.reserve(gstate.objective->per_columns.size());
        for (auto &col : gstate.objective->per_columns) {
            key_exprs.push_back(TransformToChunkExpression(*col, context));
        }
        BuildGroupIds(key_exprs, context, gstate.data, num_rows,
                      obj_row_is_included, /*null_excludes=*/true,
                      solver_input.objective_row_group_ids,
                      solver_input.objective_num_groups);
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

        // Base (unscaled) RHS. For AVG(x) <> K we store the original K in rhs_values and
        // multiply by group size per non-empty group below.
        double base_rhs = ec.rhs_values[0];

        for (idx_t g = 0; g < num_groups_to_process; g++) {
            if (group_rows[g].empty()) {
                continue;
            }

            double rhs = base_rhs;
            if (ec.ne_avg_rhs_scale) {
                rhs *= static_cast<double>(group_rows[g].size());
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

    // Save objective data (needed for constraint generation in the PER MIN/MAX
    // and flat aggregate paths). Defer the deep copy of objective_coefficients
    // — which is a vector<vector<double>> sized num_terms * num_rows — until
    // we know we'll take one of those paths.
    auto saved_obj_var_indices = solver_input.objective_variable_indices;
    bool need_saved_obj =
        !saved_obj_var_indices.empty() &&
        ((per_inner_agg != ObjectiveAggregateType::NONE && solver_input.objective_num_groups > 0) ||
         flat_objective_agg != ObjectiveAggregateType::NONE);
    vector<vector<double>> saved_obj_coefficients;
    if (need_saved_obj) {
        saved_obj_coefficients = solver_input.objective_coefficients;
    }

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

    // ================================================================
    // Composed MIN/MAX constraints: additive LHS mixing SUM/AVG/MIN/MAX.
    // Each MIN/MAX term gets a global auxiliary z_k pinned by per-row
    // constraints. The outer composed constraint is emitted as a
    // RawConstraint summing SUM/AVG contributions + z_k references.
    // v1 scope: easy cases only (MAX pushed down / MIN pushed up),
    // constant RHS, no outer WHEN/PER wrappers.
    // ================================================================
    if (!composed_minmax_constraints.empty()) {
        // Helper: evaluate a Term's per-row coefficient (scaled by term.sign)
        auto EvaluateTermCoefs = [&](const Term &term) -> vector<double> {
            vector<double> coefs;
            coefs.reserve(num_rows);
            auto transformed = TransformToChunkExpression(*term.coefficient, context);
            ExpressionExecutor exec(context);
            exec.AddExpression(*transformed);
            ColumnDataScanState scan;
            gstate.data.InitializeScan(scan);
            DataChunk chunk;
            chunk.Initialize(context, gstate.data.Types());
            while (gstate.data.Scan(scan, chunk)) {
                DataChunk result;
                result.Initialize(context, {transformed->return_type});
                exec.Execute(chunk, result);
                for (idx_t r = 0; r < chunk.size(); r++) {
                    Value val = result.data[0].GetValue(r);
                    if (val.IsNull()) {
                        throw InvalidInputException(
                            "Composed MIN/MAX constraint: coefficient expression returned NULL.");
                    }
                    double d = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    if (!std::isfinite(d)) {
                        throw InvalidInputException(
                            "Composed MIN/MAX constraint: coefficient is not finite (NaN/Inf).");
                    }
                    coefs.push_back(d * term.sign);
                }
            }
            return coefs;
        };

        for (auto &spec : composed_minmax_constraints) {
            // RHS must be constant (possibly cast-wrapped) in v1.
            const Expression *rhs_inner = spec.rhs_expr.get();
            while (rhs_inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                rhs_inner = rhs_inner->Cast<BoundCastExpression>().child.get();
            }
            if (rhs_inner->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
                throw BinderException(
                    "Composed MIN/MAX in DECIDE v1 requires a constant RHS; got '%s'.",
                    spec.rhs_expr->ToString());
            }
            double rhs_val = rhs_inner->Cast<BoundConstantExpression>()
                                 .value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();

            struct TermAnalysis {
                LogicalDecide::ComposedMinMaxTerm::Kind kind;
                string agg_name;
                int sign;
                bool is_easy;
                vector<bool> filter_mask;
                vector<Term> inner_terms;
                vector<vector<double>> per_term_coefs;
                idx_t z_idx = DConstants::INVALID_INDEX;
            };
            vector<TermAnalysis> analyses;

            // Collect filter expressions for batch evaluation (one scan for all terms).
            vector<const Expression *> composed_cond_ptrs;
            for (auto &term : spec.terms) {
                if (term.filter) composed_cond_ptrs.push_back(term.filter.get());
            }
            auto composed_masks = EvaluateBooleanMasks(composed_cond_ptrs);

            idx_t mask_slot = 0;
            for (auto &term : spec.terms) {
                TermAnalysis ta;
                ta.kind = term.kind;
                ta.agg_name = term.agg_name;
                ta.sign = term.sign;
                ta.is_easy = term.is_easy;

                ExtractTerms(*term.inner_expr, ta.inner_terms);
                for (auto &inner_t : ta.inner_terms) {
                    ta.per_term_coefs.push_back(EvaluateTermCoefs(inner_t));
                }
                if (term.filter) {
                    ta.filter_mask = std::move(composed_masks[mask_slot++]);
                } else {
                    ta.filter_mask.assign(num_rows, true);
                }
                analyses.push_back(std::move(ta));
            }

            // Allocate global z_k for each MIN/MAX term. Reject hard cases in v1.
            for (auto &ta : analyses) {
                if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
                if (!ta.is_easy) {
                    throw BinderException(
                        "Composed MIN/MAX in DECIDE v1 supports only easy-direction "
                        "MIN/MAX terms (MAX pushed down by <=, MIN pushed up by >=). "
                        "The '%s' term here requires Big-M indicator linearization, "
                        "which is not yet implemented for composed expressions.",
                        ta.agg_name);
                }
                // Reject empty WHEN on composed MIN/MAX terms: without this the
                // z_k auxiliary floats free (no per-row pinning), silently
                // vacating the entire additive constraint.
                idx_t cnt = 0;
                for (bool m : ta.filter_mask) if (m) cnt++;
                RejectEmptyAggregate(cnt, ta.agg_name.c_str(), "composed constraint");
                ta.z_idx = pre_indexer.global_block_start + solver_input.num_global_vars;
                solver_input.num_global_vars += 1;
                solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
                solver_input.global_lower_bounds.push_back(-1e30);
                solver_input.global_upper_bounds.push_back(1e30);
                solver_input.global_obj_coeffs.push_back(0.0);
            }
            // Also reject empty WHEN on composed SUM/AVG terms for consistency
            // with the reject-all rule. Without the check, an empty SUM just
            // contributes 0 (vacuous but defined); an empty AVG currently
            // divides by zero at line ~3662 and skips, silently losing the term.
            for (auto &ta : analyses) {
                if (ta.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
                idx_t cnt = 0;
                for (bool m : ta.filter_mask) if (m) cnt++;
                RejectEmptyAggregate(cnt, ta.agg_name.c_str(), "composed constraint");
            }

            // Emit per-row pinning constraints for each MIN/MAX term.
            for (auto &ta : analyses) {
                if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
                bool is_max = (ta.agg_name == "max");
                // Easy case:
                //   MAX pushed down: z_k >= inner_expr  (solver drives z_k to max)
                //   MIN pushed up:   z_k <= inner_expr  (solver drives z_k to min)
                char sense = is_max ? '>' : '<';
                for (idx_t row = 0; row < num_rows; row++) {
                    if (!ta.filter_mask[row]) continue;
                    SolverInput::RawConstraint rc;
                    rc.indices.push_back((int)ta.z_idx);
                    rc.coefficients.push_back(1.0);
                    double row_rhs = 0.0;
                    for (idx_t it = 0; it < ta.inner_terms.size(); it++) {
                        auto &inner_t = ta.inner_terms[it];
                        double coef = ta.per_term_coefs[it][row];
                        if (inner_t.variable_index == DConstants::INVALID_INDEX) {
                            row_rhs += coef;
                        } else {
                            idx_t abs_idx = pre_indexer.Get(inner_t.variable_index, row);
                            rc.indices.push_back((int)abs_idx);
                            rc.coefficients.push_back(-coef);
                        }
                    }
                    rc.sense = sense;
                    rc.rhs = row_rhs;
                    solver_input.global_constraints.push_back(std::move(rc));
                }
            }

            // Build the outer composed RawConstraint
            std::unordered_map<int, double> outer_accum;
            double outer_rhs = rhs_val;
            for (auto &ta : analyses) {
                if (ta.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
                    outer_accum[(int)ta.z_idx] += (double)ta.sign;
                } else {
                    // SUM/AVG term. For AVG, divide by filtered row count.
                    double avg_divisor = 1.0;
                    if (ta.agg_name == "avg") {
                        idx_t cnt = 0;
                        for (idx_t r = 0; r < num_rows; r++) {
                            if (ta.filter_mask[r]) cnt++;
                        }
                        if (cnt == 0) {
                            // Empty aggregate — contributes 0; skip.
                            continue;
                        }
                        avg_divisor = static_cast<double>(cnt);
                    }
                    for (idx_t it = 0; it < ta.inner_terms.size(); it++) {
                        auto &inner_t = ta.inner_terms[it];
                        for (idx_t row = 0; row < num_rows; row++) {
                            if (!ta.filter_mask[row]) continue;
                            double coef = ta.per_term_coefs[it][row] * (double)ta.sign / avg_divisor;
                            if (inner_t.variable_index == DConstants::INVALID_INDEX) {
                                outer_rhs -= coef;
                            } else {
                                int abs_idx = (int)pre_indexer.Get(inner_t.variable_index, row);
                                outer_accum[abs_idx] += coef;
                            }
                        }
                    }
                }
            }

            SolverInput::RawConstraint outer;
            for (auto &p : outer_accum) {
                if (p.second != 0.0) {
                    outer.indices.push_back(p.first);
                    outer.coefficients.push_back(p.second);
                }
            }
            switch (spec.outer_cmp) {
            case ExpressionType::COMPARE_LESSTHANOREQUALTO:
            case ExpressionType::COMPARE_LESSTHAN:
                outer.sense = '<';
                outer.rhs = outer_rhs;
                break;
            case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
            case ExpressionType::COMPARE_GREATERTHAN:
                outer.sense = '>';
                outer.rhs = outer_rhs;
                break;
            default:
                throw InternalException("Composed MIN/MAX: unexpected comparison type.");
            }
            solver_input.global_constraints.push_back(std::move(outer));
        }
    }

    // ================================================================
    // Composed MIN/MAX objective: `MAXIMIZE|MINIMIZE T1 + T2 + ...`
    // Each MIN/MAX term gets a global z_k pinned by per-row constraints;
    // SUM/AVG terms populate objective_coefficients. v1: easy-direction
    // terms only, no outer PER/WHEN on the objective.
    // ================================================================
    if (!composed_minmax_objective_terms.empty()) {
        auto EvaluateTermCoefsObj = [&](const Term &term) -> vector<double> {
            vector<double> coefs;
            coefs.reserve(num_rows);
            auto transformed = TransformToChunkExpression(*term.coefficient, context);
            ExpressionExecutor exec(context);
            exec.AddExpression(*transformed);
            ColumnDataScanState scan;
            gstate.data.InitializeScan(scan);
            DataChunk chunk;
            chunk.Initialize(context, gstate.data.Types());
            while (gstate.data.Scan(scan, chunk)) {
                DataChunk result;
                result.Initialize(context, {transformed->return_type});
                exec.Execute(chunk, result);
                for (idx_t r = 0; r < chunk.size(); r++) {
                    Value val = result.data[0].GetValue(r);
                    if (val.IsNull()) {
                        throw InvalidInputException(
                            "Composed MIN/MAX objective: coefficient expression returned NULL.");
                    }
                    double d = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    if (!std::isfinite(d)) {
                        throw InvalidInputException(
                            "Composed MIN/MAX objective: coefficient is not finite.");
                    }
                    coefs.push_back(d * term.sign);
                }
            }
            return coefs;
        };

        // Clear any existing objective terms — the placeholder constant produced
        // none, but be defensive in case other paths populated them.
        solver_input.objective_coefficients.clear();
        solver_input.objective_variable_indices.clear();

        struct ObjTermAnalysis {
            LogicalDecide::ComposedMinMaxTerm::Kind kind;
            string agg_name;
            int sign;
            bool is_easy;
            vector<bool> filter_mask;
            vector<Term> inner_terms;
            vector<vector<double>> per_term_coefs;
            idx_t z_idx = DConstants::INVALID_INDEX;
        };
        vector<ObjTermAnalysis> obj_analyses;

        // Batch-evaluate all per-term filter conditions for composed objective terms.
        {
            vector<const Expression *> obj_comp_cond_ptrs;
            for (auto &term : composed_minmax_objective_terms) {
                if (term.filter) obj_comp_cond_ptrs.push_back(term.filter.get());
            }
            auto obj_comp_masks = EvaluateBooleanMasks(obj_comp_cond_ptrs);

            idx_t mask_slot = 0;
            for (auto &term : composed_minmax_objective_terms) {
                ObjTermAnalysis ta;
                ta.kind = term.kind;
                ta.agg_name = term.agg_name;
                ta.sign = term.sign;
                ta.is_easy = term.is_easy;
                ExtractTerms(*term.inner_expr, ta.inner_terms);
                for (auto &inner_t : ta.inner_terms) {
                    ta.per_term_coefs.push_back(EvaluateTermCoefsObj(inner_t));
                }
                if (term.filter) {
                    ta.filter_mask = std::move(obj_comp_masks[mask_slot++]);
                } else {
                    ta.filter_mask.assign(num_rows, true);
                }
                obj_analyses.push_back(std::move(ta));
            }
        }

        // Allocate z_k per MIN/MAX term. v1 rejects hard direction.
        for (auto &ta : obj_analyses) {
            if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
            if (!ta.is_easy) {
                throw BinderException(
                    "Composed MIN/MAX objective in DECIDE v1 supports only "
                    "easy-direction terms (MAXIMIZE+MIN or MINIMIZE+MAX). "
                    "The '%s' term here requires indicator linearization, "
                    "which is not yet implemented for composed objectives.",
                    ta.agg_name);
            }
            // Reject empty WHEN on composed MIN/MAX objective terms: without
            // this the z_k floats free and the objective silently ignores the
            // missing piece.
            idx_t cnt = 0;
            for (bool m : ta.filter_mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, ta.agg_name.c_str(), "composed objective");
            ta.z_idx = pre_indexer.global_block_start + solver_input.num_global_vars;
            solver_input.num_global_vars += 1;
            solver_input.global_variable_types.push_back(LogicalType::DOUBLE);
            solver_input.global_lower_bounds.push_back(-1e30);
            solver_input.global_upper_bounds.push_back(1e30);
            solver_input.global_obj_coeffs.push_back(0.0);
        }
        // Mirror the SUM/AVG empty-set rejection from the composed constraint path.
        for (auto &ta : obj_analyses) {
            if (ta.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
            idx_t cnt = 0;
            for (bool m : ta.filter_mask) if (m) cnt++;
            RejectEmptyAggregate(cnt, ta.agg_name.c_str(), "composed objective");
        }

        // Pinning constraints for MIN/MAX terms.
        for (auto &ta : obj_analyses) {
            if (ta.kind != LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) continue;
            bool is_max = (ta.agg_name == "max");
            // MAXIMIZE+MIN: z_k <= expr_i per row (solver drives z_k up to min)
            // MINIMIZE+MAX: z_k >= expr_i per row (solver drives z_k down to max)
            char sense = is_max ? '>' : '<';
            for (idx_t row = 0; row < num_rows; row++) {
                if (!ta.filter_mask[row]) continue;
                SolverInput::RawConstraint rc;
                rc.indices.push_back((int)ta.z_idx);
                rc.coefficients.push_back(1.0);
                double row_rhs = 0.0;
                for (idx_t it = 0; it < ta.inner_terms.size(); it++) {
                    auto &inner_t = ta.inner_terms[it];
                    double coef = ta.per_term_coefs[it][row];
                    if (inner_t.variable_index == DConstants::INVALID_INDEX) {
                        row_rhs += coef;
                    } else {
                        idx_t abs_idx = pre_indexer.Get(inner_t.variable_index, row);
                        rc.indices.push_back((int)abs_idx);
                        rc.coefficients.push_back(-coef);
                    }
                }
                rc.sense = sense;
                rc.rhs = row_rhs;
                solver_input.global_constraints.push_back(std::move(rc));
            }
        }

        // Populate objective coefficients. For MIN/MAX terms, the obj coef on z_k
        // is ta.sign (i.e., sign×1.0); set via global_obj_coeffs. For SUM/AVG
        // terms, accumulate per-row linear coefficients into objective_coefficients
        // keyed by decide variable.
        // Accumulator: decide_var_index -> per-row coefficient vector.
        std::unordered_map<idx_t, vector<double>> obj_coef_accum;
        for (auto &ta : obj_analyses) {
            if (ta.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
                // The z_k's obj coef is ta.sign (the MIN/MAX term's sign in the additive sum).
                idx_t gslot = ta.z_idx - pre_indexer.global_block_start;
                solver_input.global_obj_coeffs[gslot] = (double)ta.sign;
            } else {
                double avg_divisor = 1.0;
                if (ta.agg_name == "avg") {
                    idx_t cnt = 0;
                    for (idx_t r = 0; r < num_rows; r++) if (ta.filter_mask[r]) cnt++;
                    if (cnt == 0) continue;
                    avg_divisor = (double)cnt;
                }
                for (idx_t it = 0; it < ta.inner_terms.size(); it++) {
                    auto &inner_t = ta.inner_terms[it];
                    if (inner_t.variable_index == DConstants::INVALID_INDEX) continue;
                    auto &dst = obj_coef_accum[inner_t.variable_index];
                    if (dst.empty()) dst.assign(num_rows, 0.0);
                    for (idx_t row = 0; row < num_rows; row++) {
                        if (!ta.filter_mask[row]) continue;
                        dst[row] += ta.per_term_coefs[it][row] * (double)ta.sign / avg_divisor;
                    }
                }
            }
        }
        for (auto &p : obj_coef_accum) {
            solver_input.objective_variable_indices.push_back(p.first);
            solver_input.objective_coefficients.push_back(std::move(p.second));
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
