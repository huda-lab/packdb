#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Data Structures for Expression Term Extraction
//===--------------------------------------------------------------------===//

//! Represents a single term: variable_index * coefficient_expression
//! variable_index can be DConstants::INVALID_INDEX for constant terms.
//! Used in both linear and quadratic (inner) expressions.
struct Term {
    idx_t variable_index;              // Which DECIDE variable (or INVALID_INDEX for constants)
    unique_ptr<Expression> coefficient; // Row-varying expression to evaluate later
    int sign = 1;                       // +1 or -1, applied at coefficient evaluation time

    Term(idx_t var_idx, unique_ptr<Expression> coef, int s = 1)
        : variable_index(var_idx), coefficient(std::move(coef)), sign(s) {}
};

//! Represents a complete constraint after term extraction
struct DecideConstraint {
    vector<Term> lhs_terms;              // All additive terms from LHS
    unique_ptr<Expression> rhs_expr;     // RHS expression (may contain aggregates)
    ExpressionType comparison_type;      // COMPARE_LESSTHANOREQUALTO or GREATERTHANOREQUALTO
    bool lhs_is_aggregate = false;       // True if original LHS was an aggregate (e.g., SUM(...))
    bool was_avg_rewrite = false;        // True if this aggregate was originally AVG (RHS needs scaling)
    idx_t minmax_indicator_idx = DConstants::INVALID_INDEX;  // Indicator var idx for hard MIN/MAX
    string minmax_agg_type;              // "min" or "max" (empty if not minmax)
    idx_t ne_indicator_idx = DConstants::INVALID_INDEX;      // Indicator var idx for not-equal
    unique_ptr<Expression> when_condition;           // PackDB: optional WHEN condition (nullptr = unconditional)
    vector<unique_ptr<Expression>> per_columns;     // PackDB: optional PER grouping columns (empty = no grouping)

    DecideConstraint() = default;
};

//! Represents the objective function after term extraction.
//! Supports both linear objectives (terms only) and quadratic objectives
//! of the form MINIMIZE SUM((linear_expr)^2) + linear_terms.
struct Objective {
    vector<Term> terms;                    // Linear objective terms
    unique_ptr<Expression> when_condition; // PackDB: optional WHEN condition (nullptr = unconditional)
    vector<unique_ptr<Expression>> per_columns; // PackDB: optional PER grouping columns (empty = no grouping)

    //! Quadratic objective: the inner linear expression of each SUM(POWER(expr, 2)) term.
    //! When non-empty, the objective includes a quadratic component: SUM((inner_expr)^2).
    //! Convexity is guaranteed by syntax: only squared linear expressions are accepted,
    //! producing Q = A^T A which is always positive semidefinite.
    vector<Term> squared_terms;
    bool has_quadratic = false;

    Objective() = default;
};

//===--------------------------------------------------------------------===//
// PhysicalDecide Operator
//===--------------------------------------------------------------------===//

//! PhysicalDecide represents a blocking operator that solves an ILP over its
//! entire input before producing output.
class PhysicalDecide : public PhysicalOperator {
public:
    static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::DECIDE;

public:
    PhysicalDecide(vector<LogicalType> types, idx_t estimated_cardinality, 
                    unique_ptr<PhysicalOperator> child, idx_t decide_index, 
                    vector<unique_ptr<Expression>> decide_variables,
                    unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                    unique_ptr<Expression> decide_objective);

    // The table index for the new columns
    idx_t decide_index;

    // The variables to be decided (e.g., x, y)
    vector<unique_ptr<Expression>> decide_variables;

    // The bound constraints expression
    unique_ptr<Expression> decide_constraints;

    // The optimization sense (MINIMIZE or MAXIMIZE)
    DecideSense decide_sense;

    // The bound objective function expression
    unique_ptr<Expression> decide_objective;

    // Number of auxiliary variables (e.g. from ABS linearization) at the end of decide_variables
    idx_t num_auxiliary_vars = 0;

    // Links from COUNT indicator variables to their original variables (indicator_idx -> original_idx)
    vector<pair<idx_t, idx_t>> count_indicator_links;

    // Indices of auxiliary indicator variables for not-equal (<>) constraints
    vector<idx_t> ne_indicator_indices;

    // Links from MIN/MAX indicator variables: (agg_name "min"/"max", indicator_idx)
    vector<pair<string, idx_t>> minmax_indicator_links;

    // --- MIN/MAX objective metadata (set by DecideOptimizer::RewriteMinMaxObjective) ---

    // Flat (non-PER) objective: original aggregate type before rewrite to SUM
    ObjectiveAggregateType flat_objective_agg = ObjectiveAggregateType::NONE;
    // Pre-computed: true if easy formulation (MAXIMIZE+MIN or MINIMIZE+MAX)
    bool flat_objective_is_easy = false;

    // PER nested objective: OUTER(INNER(expr)) PER col
    ObjectiveAggregateType per_inner_agg = ObjectiveAggregateType::NONE;
    ObjectiveAggregateType per_outer_agg = ObjectiveAggregateType::NONE;
    // Pre-computed easy/hard at each level (only meaningful when agg is MIN_AGG or MAX_AGG)
    bool per_inner_is_easy = false;
    bool per_outer_is_easy = false;
    // True if inner aggregate was originally AVG (coefficients need 1/n_g scaling)
    bool per_inner_was_avg = false;

public:
    // Source interface
    unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
    SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;

    // Sink interface
    unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
    unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
    SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
    SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
    SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                              OperatorSinkFinalizeInput &input) const override;

    bool IsSource() const override {
        return true;
    }

    bool IsSink() const override {
        return true;
    }
    bool ParallelSink() const override {
        return true;
    }

public:
    string GetName() const override;
    InsertionOrderPreservingMap<string> ParamsToString() const override;

public:
    //! Helper methods for expression analysis (used by DecideGlobalSinkState)

    //! Find a DECIDE variable in an expression tree
    //! Returns variable index or DConstants::INVALID_INDEX if not found
    idx_t FindDecideVariable(const Expression &expr) const;

    //! Check if expression contains a specific DECIDE variable
    bool ContainsVariable(const Expression &expr, idx_t var_idx) const;

    //! Extract coefficient expression, removing the specified variable
    //! For example: from "x * 5 * l_tax", removes x and returns "5 * l_tax"
    unique_ptr<Expression> ExtractCoefficientWithoutVariable(const Expression &expr, idx_t var_idx) const;

    //! Main visitor: extract all terms from a SUM argument
    //! Handles + operators (recursively), * operators (extract var and coef), constants
    void ExtractTerms(const Expression &expr, vector<Term> &out_terms) const;
};

} // namespace duckdb