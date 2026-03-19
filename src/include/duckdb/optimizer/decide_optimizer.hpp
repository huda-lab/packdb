//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/optimizer/decide_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
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
	//! Constraints: tagged with AVG_REWRITE_TAG so execution can scale RHS by row count.
	//! Objectives: pure replacement (dividing by constant N doesn't change argmax/argmin).
	void RewriteAvgToSum(LogicalDecide &decide);

	//! Helper: recursively walk a bound expression tree, replacing AVG aggregates with SUM.
	//! When is_objective is false, the replacement is tagged with AVG_REWRITE_TAG.
	void RewriteAvgInExpression(unique_ptr<Expression> &expr, bool is_objective);

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

	//! Helper: recursively find BoundFunctionExpression for ABS over decide vars,
	//! replace with auxiliary variable references, and collect (aux_idx, inner_expr) pairs.
	void FindAndReplaceAbs(unique_ptr<Expression> &expr, LogicalDecide &decide,
	                       vector<pair<idx_t, unique_ptr<Expression>>> &abs_pairs);

	//! Helper: append a constraint to the decide constraint tree via AND conjunction
	static void AppendConstraint(LogicalDecide &decide, unique_ptr<Expression> constraint);

	Optimizer &optimizer;
};

} // namespace duckdb
