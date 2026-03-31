#include "duckdb/optimizer/decide_optimizer.hpp"

#include <cstdlib>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

static ObjectiveAggregateType StrToAggType(const string &name) {
	if (name == "sum") return ObjectiveAggregateType::SUM;
	if (name == "min") return ObjectiveAggregateType::MIN_AGG;
	if (name == "max") return ObjectiveAggregateType::MAX_AGG;
	return ObjectiveAggregateType::NONE;
}

DecideOptimizer::DecideOptimizer(Optimizer &optimizer) : optimizer(optimizer) {
}

unique_ptr<LogicalOperator> DecideOptimizer::Optimize(unique_ptr<LogicalOperator> op) {
	// Recurse into children first (bottom-up)
	for (auto &child : op->children) {
		child = Optimize(std::move(child));
	}

	// If this is a LogicalDecide node, apply DECIDE-specific optimizations
	if (op->type == LogicalOperatorType::LOGICAL_DECIDE) {
		auto &decide = op->Cast<LogicalDecide>();
		OptimizeDecide(decide);
	}

	return op;
}

void DecideOptimizer::OptimizeDecide(LogicalDecide &decide) {
	bool bench = std::getenv("PACKDB_BENCH") != nullptr;
	Profiler timer;
	if (bench) {
		timer.Start();
	}

	RewriteAbs(decide);          // Must run first: creates aux vars replacing ABS nodes
	RewriteMinMax(decide);       // Classify + rewrite min/max (creates indicators and SUM nodes)
	RewriteNotEqual(decide);
	RewriteCountToSum(decide);
	RewriteAvgToSum(decide);

	if (bench) {
		timer.End();
		fprintf(stderr, "PACKDB_BENCH: optimizer_ms=%.2f\n", timer.Elapsed() * 1000.0);
	}
}

void DecideOptimizer::RewriteNotEqual(LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	// Walk the bound constraint tree and find all COMPARE_NOTEQUAL expressions.
	// For each one, create an auxiliary BOOLEAN indicator variable.
	// The constraint expression itself is NOT modified — the physical operator
	// matches COMPARE_NOTEQUAL constraints with ne_indicator_indices at execution time.
	FindNotEqualConstraints(*decide.decide_constraints, decide);
}

void DecideOptimizer::FindNotEqualConstraints(Expression &expr, LogicalDecide &decide) {
	// Handle WHEN/PER wrappers: BoundConjunctionExpression with alias tag
	// Recurse into child[0] (the actual constraint)
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (conj.alias == WHEN_CONSTRAINT_TAG || conj.alias == PER_CONSTRAINT_TAG) {
			// child[0] is the wrapped constraint
			if (!conj.children.empty()) {
				FindNotEqualConstraints(*conj.children[0], decide);
			}
			return;
		}
		// Regular AND conjunction — recurse into all children
		for (auto &child : conj.children) {
			FindNotEqualConstraints(*child, decide);
		}
		return;
	}

	// Found a not-equal comparison — create an indicator variable
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		if (comp.type == ExpressionType::COMPARE_NOTEQUAL) {
			// Create auxiliary BOOLEAN indicator variable
			idx_t ind_idx = decide.decide_variables.size();
			string ind_name = "__ne_ind_" + to_string(decide.ne_indicator_indices.size()) + "__";
			auto ind_var = make_uniq<BoundColumnRefExpression>(
			    ind_name, LogicalType::BOOLEAN, ColumnBinding(decide.decide_index, ind_idx));
			decide.decide_variables.push_back(std::move(ind_var));
			decide.ne_indicator_indices.push_back(ind_idx);
			decide.num_auxiliary_vars++;
			if (!decide.variable_entity_scope.empty()) {
				decide.variable_entity_scope.push_back(DConstants::INVALID_INDEX);
			}
			// Tag the comparison with the indicator index for direct matching
			comp.alias = string(NE_INDICATOR_TAG_PREFIX) + to_string(ind_idx) + "__";
		}
	}
}

// ---------------------------------------------------------------------------
// COUNT → SUM rewrite
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteCountToSum(LogicalDecide &decide) {
	case_insensitive_map_t<idx_t> count_indicator_map;
	if (decide.decide_constraints) {
		RewriteCountInExpression(decide.decide_constraints, decide, count_indicator_map);
	}
	if (decide.decide_objective) {
		RewriteCountInExpression(decide.decide_objective, decide, count_indicator_map);
	}
}

void DecideOptimizer::RewriteCountInExpression(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                               case_insensitive_map_t<idx_t> &count_indicator_map) {
	if (!expr) {
		return;
	}

	// Check if this node is a COUNT aggregate over a decide variable
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = expr->Cast<BoundAggregateExpression>();
		if (StringUtil::CIEquals(agg.function.name, "count") && agg.children.size() == 1) {
			auto &child = agg.children[0];
			if (child->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				auto &colref = child->Cast<BoundColumnRefExpression>();

				// Find the variable index in decide_variables
				idx_t var_idx = DConstants::INVALID_INDEX;
				for (idx_t i = 0; i < decide.decide_variables.size(); i++) {
					auto &var = decide.decide_variables[i]->Cast<BoundColumnRefExpression>();
					if (var.binding == colref.binding) {
						var_idx = i;
						break;
					}
				}
				if (var_idx == DConstants::INVALID_INDEX) {
					return; // Not a decide variable — shouldn't happen after binder validation
				}

				auto &decide_var = decide.decide_variables[var_idx]->Cast<BoundColumnRefExpression>();

				if (decide_var.return_type == LogicalType::DOUBLE) {
					throw InternalException("COUNT(%s) requires a BOOLEAN or INTEGER decision variable. "
					                        "For REAL variables, COUNT is not yet supported.",
					                        decide_var.alias);
				}

				if (decide_var.return_type == LogicalType::BOOLEAN) {
					// BOOLEAN: COUNT(x) = SUM(x) — replace with SUM over same variable
					vector<unique_ptr<Expression>> sum_children;
					sum_children.push_back(child->Copy());
					expr = optimizer.BindAggregateFunction("sum", std::move(sum_children));
				} else {
					// INTEGER: COUNT(x) → SUM(indicator)
					string var_name = decide_var.alias;
					idx_t indicator_idx;

					auto it = count_indicator_map.find(var_name);
					if (it != count_indicator_map.end()) {
						// Reuse existing indicator for same variable
						indicator_idx = it->second;
					} else {
						// Create new BOOLEAN indicator variable
						indicator_idx = decide.decide_variables.size();
						string ind_name = "__count_ind_" + var_name + "__";
						auto ind_var = make_uniq<BoundColumnRefExpression>(
						    ind_name, LogicalType::BOOLEAN,
						    ColumnBinding(decide.decide_index, indicator_idx));
						decide.decide_variables.push_back(std::move(ind_var));
						decide.num_auxiliary_vars++;
						if (!decide.variable_entity_scope.empty()) {
							decide.variable_entity_scope.push_back(DConstants::INVALID_INDEX);
						}
						count_indicator_map.emplace(var_name, indicator_idx);

						// Record the indicator→original link (once per unique indicator)
						decide.count_indicator_links.emplace_back(indicator_idx, var_idx);

						// Generate z <= x constraint (forces z=0 when x=0)
						// No Big-M dependency — fully algebraic, follows ABS linearization pattern.
						// The companion constraint x <= M*z (forces z=1 when x>0) remains in
						// physical execution because M depends on runtime variable bounds.
						auto z_ref_c = make_uniq<BoundColumnRefExpression>(
						    ind_name, LogicalType::BOOLEAN,
						    ColumnBinding(decide.decide_index, indicator_idx));
						auto x_ref_c = make_uniq<BoundColumnRefExpression>(
						    decide_var.alias, decide_var.return_type, decide_var.binding);
						auto c_zlex = make_uniq<BoundComparisonExpression>(
						    ExpressionType::COMPARE_LESSTHANOREQUALTO,
						    std::move(z_ref_c), std::move(x_ref_c));
						AppendConstraint(decide, std::move(c_zlex));
					}

					// Create SUM(indicator) expression
					auto &ind_var = decide.decide_variables[indicator_idx]->Cast<BoundColumnRefExpression>();
					auto ind_ref = make_uniq<BoundColumnRefExpression>(
					    ind_var.alias, ind_var.return_type, ind_var.binding);
					vector<unique_ptr<Expression>> sum_children;
					sum_children.push_back(std::move(ind_ref));
					expr = optimizer.BindAggregateFunction("sum", std::move(sum_children));
				}
				return; // Node replaced, no need to recurse into it
			}
		}
	}

	// Recurse into children
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		RewriteCountInExpression(child, decide, count_indicator_map);
	});
}

// ---------------------------------------------------------------------------
// AVG → SUM rewrite
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAvgToSum(LogicalDecide &decide) {
	if (decide.decide_constraints) {
		RewriteAvgInExpression(decide.decide_constraints, /*is_objective=*/false);
	}
	if (decide.decide_objective) {
		RewriteAvgInExpression(decide.decide_objective, /*is_objective=*/true);
	}
}

void DecideOptimizer::RewriteAvgInExpression(unique_ptr<Expression> &expr, bool is_objective) {
	if (!expr) {
		return;
	}

	// Check if this node is an AVG aggregate — may be wrapped in a BOUND_CAST
	Expression *inner = expr.get();
	bool has_cast = false;
	if (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		has_cast = true;
		inner = inner->Cast<BoundCastExpression>().child.get();
	}

	if (inner->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = inner->Cast<BoundAggregateExpression>();
		if (StringUtil::CIEquals(agg.function.name, "avg") && agg.children.size() == 1) {
			// Replace AVG(expr) with SUM(expr)
			vector<unique_ptr<Expression>> sum_children;
			sum_children.push_back(agg.children[0]->Copy());
			auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));

			if (!is_objective) {
				// Tag so execution layer knows to scale RHS by row count
				new_sum->alias = AVG_REWRITE_TAG;
			}

			if (has_cast) {
				// Preserve the cast wrapper — update its child
				auto &cast_expr = expr->Cast<BoundCastExpression>();
				cast_expr.child = std::move(new_sum);
			} else {
				expr = std::move(new_sum);
			}
			return;
		}
	}

	// Recurse into children
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		RewriteAvgInExpression(child, is_objective);
	});
}

// ---------------------------------------------------------------------------
// Shared helper
// ---------------------------------------------------------------------------

//! Check if a bound expression references any decide variable (by table_index match)
static bool BoundExprReferencesDecideVar(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return colref.binding.table_index == decide_index;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = BoundExprReferencesDecideVar(child, decide_index);
		}
	});
	return found;
}

// ---------------------------------------------------------------------------
// MIN/MAX linearization
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteMinMax(LogicalDecide &decide) {
	RewriteMinMaxConstraints(decide);
	RewriteMinMaxObjective(decide);
}

void DecideOptimizer::RewriteMinMaxConstraints(LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	vector<unique_ptr<Expression>> new_constraints;
	bool was_easy = false;
	RewriteMinMaxInConstraint(decide.decide_constraints, decide, new_constraints, was_easy);

	// Append generated constraints (from equality splitting) to the constraint tree
	for (auto &nc : new_constraints) {
		AppendConstraint(decide, std::move(nc));
	}
}

void DecideOptimizer::RewriteMinMaxInConstraint(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                                vector<unique_ptr<Expression>> &new_constraints,
                                                bool &out_was_easy) {
	if (!expr) {
		return;
	}

	// Handle WHEN/PER wrappers
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr->Cast<BoundConjunctionExpression>();
		if (conj.alias == WHEN_CONSTRAINT_TAG) {
			// Recurse into the wrapped constraint (child[0])
			if (!conj.children.empty()) {
				RewriteMinMaxInConstraint(conj.children[0], decide, new_constraints, out_was_easy);
			}
			return;
		}
		if (conj.alias == PER_CONSTRAINT_TAG) {
			// Recurse into the wrapped constraint (child[0])
			if (!conj.children.empty()) {
				RewriteMinMaxInConstraint(conj.children[0], decide, new_constraints, out_was_easy);
				// If PER wrapped an easy case, strip PER (per-row makes PER redundant)
				if (out_was_easy) {
					expr = std::move(conj.children[0]);
				}
			}
			return;
		}
		// Regular AND conjunction — recurse into all children
		for (auto &child : conj.children) {
			bool child_easy = false;
			RewriteMinMaxInConstraint(child, decide, new_constraints, child_easy);
		}
		return;
	}

	// Check for comparison with MIN/MAX on LHS
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
		return;
	}
	auto &comp = expr->Cast<BoundComparisonExpression>();

	// Unwrap any BoundCastExpression on the LHS
	Expression *lhs = comp.left.get();
	while (lhs->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		lhs = lhs->Cast<BoundCastExpression>().child.get();
	}

	if (lhs->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	auto &agg = lhs->Cast<BoundAggregateExpression>();
	auto fname = StringUtil::Lower(agg.function.name);
	if (fname != "min" && fname != "max") {
		return;
	}
	if (agg.children.size() != 1) {
		return;
	}
	// Guard: only rewrite MIN/MAX over decide variables
	if (!BoundExprReferencesDecideVar(*agg.children[0], decide.decide_index)) {
		return;
	}

	bool is_max = (fname == "max");
	auto cmp_type = comp.type;

	// Classify: easy vs hard
	bool is_easy = false;
	if (is_max && (cmp_type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	               cmp_type == ExpressionType::COMPARE_LESSTHAN)) {
		is_easy = true; // MAX(expr) <= K → every row: expr <= K
	}
	if (!is_max && (cmp_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	                cmp_type == ExpressionType::COMPARE_GREATERTHAN)) {
		is_easy = true; // MIN(expr) >= K → every row: expr >= K
	}

	bool is_hard = false;
	if (is_max && (cmp_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	               cmp_type == ExpressionType::COMPARE_GREATERTHAN)) {
		is_hard = true; // MAX(expr) >= K → need indicator
	}
	if (!is_max && (cmp_type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	                cmp_type == ExpressionType::COMPARE_LESSTHAN)) {
		is_hard = true; // MIN(expr) <= K → need indicator
	}

	if (cmp_type == ExpressionType::COMPARE_NOTEQUAL) {
		throw BinderException("DECIDE does not support <> comparison with MIN/MAX aggregates.");
	}

	if (cmp_type == ExpressionType::COMPARE_EQUAL) {
		// Equality: split into easy + hard parts
		// MAX(expr) = K → (expr <= K) AND (MAX(expr) >= K)
		// MIN(expr) = K → (expr >= K) AND (MIN(expr) <= K)

		// Easy part: per-row bound
		auto easy_cmp_type = is_max ? ExpressionType::COMPARE_LESSTHANOREQUALTO
		                            : ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		auto easy = make_uniq<BoundComparisonExpression>(
		    easy_cmp_type,
		    agg.children[0]->Copy(), comp.right->Copy());
		new_constraints.push_back(std::move(easy));

		// Hard part: create indicator
		auto hard_cmp_type = is_max ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
		                            : ExpressionType::COMPARE_LESSTHANOREQUALTO;
		idx_t ind_idx = decide.decide_variables.size();
		string ind_name = "__minmax_ind_" + to_string(decide.minmax_indicator_links.size()) + "__";
		auto ind_var = make_uniq<BoundColumnRefExpression>(
		    ind_name, LogicalType::BOOLEAN, ColumnBinding(decide.decide_index, ind_idx));
		decide.decide_variables.push_back(std::move(ind_var));
		decide.num_auxiliary_vars++;
		if (!decide.variable_entity_scope.empty()) {
			decide.variable_entity_scope.push_back(DConstants::INVALID_INDEX);
		}
		decide.minmax_indicator_links.emplace_back(fname, ind_idx);

		// Replace MIN/MAX with SUM for the hard part, tagged with indicator index
		vector<unique_ptr<Expression>> sum_children;
		sum_children.push_back(agg.children[0]->Copy());
		auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
		new_sum->alias = string(MINMAX_INDICATOR_TAG_PREFIX) + to_string(ind_idx) + "_" + fname + "__";
		comp.left = std::move(new_sum);
		comp.type = hard_cmp_type;
		return;
	}

	if (is_easy) {
		// Easy case: strip the aggregate, make it per-row
		// MAX(expr) <= K → expr <= K
		// MIN(expr) >= K → expr >= K
		comp.left = agg.children[0]->Copy();
		out_was_easy = true;
		return;
	}

	if (is_hard) {
		// Hard case: create indicator variable for Big-M linearization
		idx_t ind_idx = decide.decide_variables.size();
		string ind_name = "__minmax_ind_" + to_string(decide.minmax_indicator_links.size()) + "__";
		auto ind_var = make_uniq<BoundColumnRefExpression>(
		    ind_name, LogicalType::BOOLEAN, ColumnBinding(decide.decide_index, ind_idx));
		decide.decide_variables.push_back(std::move(ind_var));
		decide.num_auxiliary_vars++;
		if (!decide.variable_entity_scope.empty()) {
			decide.variable_entity_scope.push_back(DConstants::INVALID_INDEX);
		}
		decide.minmax_indicator_links.emplace_back(fname, ind_idx);

		// Rewrite: replace MIN/MAX with SUM, tagged with indicator index
		vector<unique_ptr<Expression>> sum_children;
		sum_children.push_back(agg.children[0]->Copy());
		auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
		new_sum->alias = string(MINMAX_INDICATOR_TAG_PREFIX) + to_string(ind_idx) + "_" + fname + "__";
		comp.left = std::move(new_sum);
		return;
	}
}

void DecideOptimizer::RewriteMinMaxObjective(LogicalDecide &decide) {
	if (!decide.decide_objective) {
		return;
	}

	// Navigate through PER and WHEN wrappers to find the actual aggregate
	unique_ptr<Expression> *obj_owner = &decide.decide_objective;
	Expression *obj_expr = decide.decide_objective.get();
	bool has_per = false;

	// Unwrap PER wrapper (outermost layer)
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj_expr->Cast<BoundConjunctionExpression>();
		if (conj.alias == PER_CONSTRAINT_TAG && !conj.children.empty()) {
			has_per = true;
			obj_owner = &conj.children[0];
			obj_expr = conj.children[0].get();
		}
	}

	// Unwrap WHEN wrapper (inside PER, if present)
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj_expr->Cast<BoundConjunctionExpression>();
		if (conj.alias == WHEN_CONSTRAINT_TAG && !conj.children.empty()) {
			obj_owner = &conj.children[0];
			obj_expr = conj.children[0].get();
		}
	}

	// Unwrap any BoundCastExpression
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = obj_expr->Cast<BoundCastExpression>();
		obj_owner = &cast.child;
		obj_expr = cast.child.get();
	}

	// Now inspect the actual aggregate
	if (obj_expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	auto &outer_agg = obj_expr->Cast<BoundAggregateExpression>();
	auto outer_name = StringUtil::Lower(outer_agg.function.name);

	// Check for nested aggregate: OUTER(INNER(expr)) where INNER is also SUM/MIN/MAX/AVG
	if (has_per && (outer_name == "sum" || outer_name == "min" || outer_name == "max" || outer_name == "avg") &&
	    outer_agg.children.size() == 1) {
		// Unwrap cast on inner child if present
		Expression *inner_expr = outer_agg.children[0].get();
		if (inner_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			inner_expr = inner_expr->Cast<BoundCastExpression>().child.get();
		}
		if (inner_expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			auto &inner_agg = inner_expr->Cast<BoundAggregateExpression>();
			auto inner_name = StringUtil::Lower(inner_agg.function.name);

			if ((inner_name == "sum" || inner_name == "min" || inner_name == "max" || inner_name == "avg") &&
			    inner_agg.children.size() == 1 &&
			    BoundExprReferencesDecideVar(*inner_agg.children[0], decide.decide_index)) {
				// Found nested pattern: set metadata
				// Map outer AVG → SUM (dividing by constant G doesn't change optimal)
				decide.per_outer_agg = (outer_name == "avg") ? ObjectiveAggregateType::SUM
				                                             : StrToAggType(outer_name);
				// Map inner AVG → SUM with flag for coefficient scaling
				if (inner_name == "avg") {
					decide.per_inner_agg = ObjectiveAggregateType::SUM;
					decide.per_inner_was_avg = true;
				} else {
					decide.per_inner_agg = StrToAggType(inner_name);
				}

				// Pre-compute easy/hard classification for inner and outer levels
				if (inner_name == "min" || inner_name == "max") {
					bool inner_is_min = (inner_name == "min");
					decide.per_inner_is_easy = (inner_is_min && decide.decide_sense == DecideSense::MAXIMIZE) ||
					                           (!inner_is_min && decide.decide_sense == DecideSense::MINIMIZE);
				}
				if (outer_name == "min" || outer_name == "max") {
					bool outer_is_min = (outer_name == "min");
					decide.per_outer_is_easy = (outer_is_min && decide.decide_sense == DecideSense::MAXIMIZE) ||
					                           (!outer_is_min && decide.decide_sense == DecideSense::MINIMIZE);
				}

				// Rewrite inner MIN/MAX/AVG → SUM for normalization
				if (inner_name == "min" || inner_name == "max" || inner_name == "avg") {
					vector<unique_ptr<Expression>> sum_children;
					sum_children.push_back(inner_agg.children[0]->Copy());
					auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
					// Replace inner aggregate within the outer
					outer_agg.children[0] = std::move(new_sum);
				}
				// Strip outer wrapper: replace OUTER(INNER(expr)) with INNER(expr)
				*obj_owner = std::move(outer_agg.children[0]);
				return;
			}
		}
	}

	// Flat MIN/MAX + PER → error (ambiguous without outer aggregate)
	if (has_per && (outer_name == "min" || outer_name == "max") &&
	    outer_agg.children.size() == 1 &&
	    BoundExprReferencesDecideVar(*outer_agg.children[0], decide.decide_index)) {
		throw BinderException(
		    "MINIMIZE/MAXIMIZE %s(...) PER is ambiguous. "
		    "With PER, use a nested aggregate to specify how per-group values are combined: "
		    "e.g., SUM(%s(...)) PER col or MAX(%s(...)) PER col.",
		    StringUtil::Upper(outer_name), StringUtil::Upper(outer_name),
		    StringUtil::Upper(outer_name));
	}

	// Flat non-PER MIN/MAX objective
	if (!has_per && (outer_name == "min" || outer_name == "max") &&
	    outer_agg.children.size() == 1 &&
	    BoundExprReferencesDecideVar(*outer_agg.children[0], decide.decide_index)) {
		decide.flat_objective_agg = StrToAggType(outer_name);
		bool is_min = (outer_name == "min");
		decide.flat_objective_is_easy = (is_min && decide.decide_sense == DecideSense::MAXIMIZE) ||
		                                (!is_min && decide.decide_sense == DecideSense::MINIMIZE);
		// Replace MIN/MAX with SUM
		vector<unique_ptr<Expression>> sum_children;
		sum_children.push_back(outer_agg.children[0]->Copy());
		*obj_owner = optimizer.BindAggregateFunction("sum", std::move(sum_children));
	}
}

// ---------------------------------------------------------------------------
// ABS linearization (self-contained: detect, replace, and generate constraints)
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAbs(LogicalDecide &decide) {
	// Phase 1: Find ABS(expr) nodes over decide vars, replace with auxiliary variables
	vector<pair<idx_t, unique_ptr<Expression>>> abs_pairs;
	if (decide.decide_constraints) {
		FindAndReplaceAbs(decide.decide_constraints, decide, abs_pairs);
	}
	if (decide.decide_objective) {
		FindAndReplaceAbs(decide.decide_objective, decide, abs_pairs);
	}

	// Phase 2: Generate linearization constraints for each auxiliary variable
	for (auto &[aux_idx, inner_expr] : abs_pairs) {
		auto &aux_var = decide.decide_variables[aux_idx];
		auto &aux_ref = aux_var->Cast<BoundColumnRefExpression>();

		// Constraint 1: aux >= inner_expr
		auto aux_ref1 = make_uniq<BoundColumnRefExpression>(
		    aux_ref.alias, aux_ref.return_type, aux_ref.binding);
		auto c1 = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		    std::move(aux_ref1), inner_expr->Copy());

		// Constraint 2: aux >= -inner_expr  (computed as 0 - inner_expr)
		auto aux_ref2 = make_uniq<BoundColumnRefExpression>(
		    aux_ref.alias, aux_ref.return_type, aux_ref.binding);
		auto neg_expr = optimizer.BindScalarFunction(
		    "-",
		    make_uniq<BoundConstantExpression>(Value::INTEGER(0)),
		    inner_expr->Copy());
		auto c2 = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		    std::move(aux_ref2), std::move(neg_expr));

		AppendConstraint(decide, std::move(c1));
		AppendConstraint(decide, std::move(c2));
	}
}

void DecideOptimizer::FindAndReplaceAbs(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                        vector<pair<idx_t, unique_ptr<Expression>>> &abs_pairs) {
	if (!expr) {
		return;
	}

	if (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr->Cast<BoundFunctionExpression>();
		if (StringUtil::CIEquals(func.function.name, "abs") && func.children.size() == 1) {
			if (BoundExprReferencesDecideVar(*func.children[0], decide.decide_index)) {
				// Create auxiliary REAL variable
				idx_t aux_idx = decide.decide_variables.size();
				string aux_name = "__abs_aux_" + to_string(abs_pairs.size()) + "__";
				auto aux_var = make_uniq<BoundColumnRefExpression>(
				    aux_name, LogicalType::DOUBLE,
				    ColumnBinding(decide.decide_index, aux_idx));
				decide.decide_variables.push_back(std::move(aux_var));
				decide.num_auxiliary_vars++;
				if (!decide.variable_entity_scope.empty()) {
					decide.variable_entity_scope.push_back(DConstants::INVALID_INDEX);
				}

				// Stash the bound inner expression for constraint generation
				abs_pairs.emplace_back(aux_idx, func.children[0]->Copy());

				// Replace ABS(inner) with aux var reference
				expr = make_uniq<BoundColumnRefExpression>(
				    aux_name, LogicalType::DOUBLE,
				    ColumnBinding(decide.decide_index, aux_idx));
				return;
			}
		}
	}

	// Recurse into children
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		FindAndReplaceAbs(child, decide, abs_pairs);
	});
}

void DecideOptimizer::AppendConstraint(LogicalDecide &decide, unique_ptr<Expression> constraint) {
	if (decide.decide_constraints) {
		auto conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		conj->children.push_back(std::move(decide.decide_constraints));
		conj->children.push_back(std::move(constraint));
		decide.decide_constraints = std::move(conj);
	} else {
		decide.decide_constraints = std::move(constraint);
	}
}

} // namespace duckdb
