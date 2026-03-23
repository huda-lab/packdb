#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Data Structures for Linear Term Extraction
//===--------------------------------------------------------------------===//

//! Represents a single linear term: variable_index * coefficient_expression
//! variable_index can be DConstants::INVALID_INDEX for constant terms
struct LinearTerm {
    idx_t variable_index;              // Which DECIDE variable (or INVALID_INDEX for constants)
    unique_ptr<Expression> coefficient; // Row-varying expression to evaluate later

    LinearTerm(idx_t var_idx, unique_ptr<Expression> coef)
        : variable_index(var_idx), coefficient(std::move(coef)) {}
};

//! Represents a complete constraint after term extraction
struct LinearConstraint {
    vector<LinearTerm> lhs_terms;       // All additive terms from LHS
    unique_ptr<Expression> rhs_expr;    // RHS expression (may contain aggregates)
    ExpressionType comparison_type;      // COMPARE_LESSTHANOREQUALTO or GREATERTHANOREQUALTO
    bool lhs_is_aggregate = false;       // True if original LHS was an aggregate (e.g., SUM(...))
    bool was_avg_rewrite = false;        // True if this aggregate was originally AVG (RHS needs scaling)
    idx_t minmax_indicator_idx = DConstants::INVALID_INDEX;  // Indicator var idx for hard MIN/MAX
    string minmax_agg_type;              // "min" or "max" (empty if not minmax)
    idx_t ne_indicator_idx = DConstants::INVALID_INDEX;      // Indicator var idx for not-equal
    unique_ptr<Expression> when_condition;           // PackDB: optional WHEN condition (nullptr = unconditional)
    vector<unique_ptr<Expression>> per_columns;     // PackDB: optional PER grouping columns (empty = no grouping)

    LinearConstraint() = default;
};

//! Represents the objective function after term extraction
struct LinearObjective {
    vector<LinearTerm> terms;           // All objective terms
    unique_ptr<Expression> when_condition; // PackDB: optional WHEN condition (nullptr = unconditional)
    vector<unique_ptr<Expression>> per_columns; // PackDB: optional PER grouping columns (empty = no grouping)

    LinearObjective() = default;
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

    //! Main visitor: extract all linear terms from a SUM argument
    //! Handles + operators (recursively), * operators (extract var and coef), constants
    void ExtractLinearTerms(const Expression &expr, vector<LinearTerm> &out_terms) const;
};

} // namespace duckdb