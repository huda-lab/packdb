//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/optimizer/decide_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {

class LogicalDecide;
class Optimizer;

//! The DecideOptimizer performs algebraic rewrites on LogicalDecide nodes.
//! This centralizes all DECIDE-specific transformations that were previously
//! scattered across the binder and physical operator.
//!
//! Current passes:
//!   - RewriteAbs: detects ABS(expr) over decide vars, creates auxiliary REAL vars,
//!     replaces ABS nodes with aux var refs, generates linearization constraints
//!   - RewriteMinMax: classifies MIN/MAX constraints as easy/hard, rewrites to per-row
//!     or SUM+indicator, handles objectives (flat and nested PER)
//!   - RewriteNotEqual: creates indicator variables for <> constraints
//!   - RewriteCountToSum: rewrites COUNT(x) → SUM(x) or SUM(indicator)
//!   - RewriteAvgToSum: rewrites AVG(expr) → SUM(expr) with alias tag for RHS scaling
//!
//! Future passes (to be migrated from binder):
//!   - IN domain rewrite
//!   - Partition-solve detection
//!   - Variable bound propagation
class DecideOptimizer {
public:
	explicit DecideOptimizer(Optimizer &optimizer);

	//! Recursively optimize the plan, transforming any LogicalDecide nodes found
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	//! Apply all DECIDE optimization passes to a LogicalDecide node
	void OptimizeDecide(LogicalDecide &decide);

	//! Rewrite not-equal (<>) constraints by creating auxiliary indicator variables.
	//! For each COMPARE_NOTEQUAL found in the constraint tree, creates a BOOLEAN
	//! indicator variable and records its index in ne_indicator_indices.
	//! The actual Big-M constraints are generated at execution time when bounds are known.
	void RewriteNotEqual(LogicalDecide &decide);

	//! Helper: recursively find COMPARE_NOTEQUAL in bound expression tree
	void FindNotEqualConstraints(Expression &expr, LogicalDecide &decide);

	//! Rewrite COUNT(var) aggregates to SUM equivalents.
	//! BOOLEAN vars: COUNT(x) → SUM(x) (direct substitution)
	//! INTEGER vars: COUNT(x) → SUM(indicator) with new BOOLEAN indicator variable
	//! REAL vars: throws error (not supported)
	void RewriteCountToSum(LogicalDecide &decide);

	//! Helper: recursively walk a bound expression tree, replacing COUNT aggregates
	//! over decision variables with SUM equivalents. Uses count_indicator_map to
	//! share indicators across multiple COUNT(x) references to the same variable.
	void RewriteCountInExpression(unique_ptr<Expression> &expr, LogicalDecide &decide,
	                              case_insensitive_map_t<idx_t> &count_indicator_map);

	//! Rewrite AVG(expr) aggregates to SUM(expr) with alias tagging.
	//! Execution scales extracted AVG terms by the active row count.
	void RewriteAvgToSum(LogicalDecide &decide);

	//! Helper: recursively walk a bound expression tree, replacing AVG aggregates with SUM.
	//! The replacement is tagged with AVG_REWRITE_TAG so coefficient evaluation can scale terms.
	void RewriteAvgInExpression(unique_ptr<Expression> &expr);

	//! Rewrite MIN/MAX aggregates in constraints and objectives.
	//! Constraints: easy cases (MAX<=K, MIN>=K) strip aggregate; hard cases create indicators.
	//! Objectives: detect flat and nested PER patterns, set metadata, rewrite to SUM.
	void RewriteMinMax(LogicalDecide &decide);

	//! Top-level constraint-side MIN/MAX rewrite
	void RewriteMinMaxConstraints(LogicalDecide &decide);

	//! Helper: recursively walk bound constraint tree, classifying and rewriting MIN/MAX.
	//! out_was_easy is set when the rewrite produced a per-row constraint (used for PER stripping).
	void RewriteMinMaxInConstraint(unique_ptr<Expression> &expr, LogicalDecide &decide,
	                               vector<unique_ptr<Expression>> &new_constraints,
	                               bool &out_was_easy);

	//! Objective-side MIN/MAX detection and rewriting.
	//! Handles flat (non-PER) and nested PER objectives.
	void RewriteMinMaxObjective(LogicalDecide &decide);

	//! Detect ABS(expr) over decide variables, create auxiliary REAL variables,
	//! replace ABS nodes with aux var references, and generate linearization constraints.
	//! For each ABS(inner): aux >= inner AND aux >= -inner.
	void RewriteAbs(LogicalDecide &decide);

	//! Rewrite bilinear products (x * y) where at least one factor is Boolean.
	//! Boolean × Boolean: w = b1 * b2, AND-linearization (w <= b1, w <= b2, w >= b1+b2-1).
	//! Boolean × Other: w = b * x, partial McCormick (w <= x at plan time,
	//!   w <= U*b and w >= x-U*(1-b) at execution time via BilinearLink).
	//! Non-Boolean × Non-Boolean: left in place for Q matrix handling in physical operator.
	void RewriteBilinear(LogicalDecide &decide);

	//! Helper: recursively find bilinear products in expression tree, replace Boolean cases
	//! with auxiliary variable references, and collect metadata.
	void FindAndReplaceBilinear(unique_ptr<Expression> &expr, LogicalDecide &decide,
	                            vector<LogicalDecide::BilinearLink> &links);

	//! Helper: walk a multiplication chain shaped as `coeff * ... * decide_var * ... * coeff`
	//! around the decide variable with index `var_idx`, and combine all non-variable factors
	//! into `coef_out`. Returns true if `expr` matches that shape (a single decide variable
	//! buried under zero or more multiplicative data coefficients). `coef_out` is null when
	//! `expr` is a bare variable reference.
	bool ExtractMultiplicativeCoefficient(const Expression &expr, idx_t decide_index,
	                                       idx_t var_idx, unique_ptr<Expression> &coef_out);

	//! Helper: recursively find BoundFunctionExpression for ABS over decide vars,
	//! replace with auxiliary variable references, and collect (aux_idx, inner_expr) pairs.
	void FindAndReplaceAbs(unique_ptr<Expression> &expr, LogicalDecide &decide,
	                       vector<pair<idx_t, unique_ptr<Expression>>> &abs_pairs);

	//! Helper: append a constraint to the decide constraint tree via AND conjunction
	static void AppendConstraint(LogicalDecide &decide, unique_ptr<Expression> constraint);

	Optimizer &optimizer;
};

} // namespace duckdb
