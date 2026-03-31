//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/planner/operator/logical_decide.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"

namespace duckdb {

//! Tracks entity-scope metadata for decision variables scoped to a base table.
//! When a variable is declared as "T.x IS BOOLEAN", it has one value per unique
//! row in table T, not per join result row.
struct EntityScopeInfo {
    //! Table alias or name used in the DECIDE declaration (e.g., "S" or "Sensors")
    string table_alias;
    //! DuckDB table index from the bind context (Binding::index)
    idx_t source_table_index;
    //! Column types for the entity key columns
    vector<LogicalType> entity_key_column_types;
    //! Physical column indices in the child's output data chunk.
    //! These are resolved during physical plan creation (plan_decide.cpp)
    //! from the logical column bindings by matching against the child's GetColumnBindings().
    vector<idx_t> entity_key_physical_indices;
    //! Logical column bindings (table_index, col_index) — used to resolve physical indices
    vector<ColumnBinding> entity_key_bindings;
    //! Which decide_variables indices are scoped to this table
    vector<idx_t> scoped_variable_indices;
};

class LogicalDecide : public LogicalOperator {
public:
    static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_DECIDE;

public:
    LogicalDecide(idx_t decide_index, vector<unique_ptr<Expression>> decide_variables,
                  unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                  unique_ptr<Expression> decide_objective);

    LogicalDecide();

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

    // Number of auxiliary variables at the end of decide_variables (created by binder and optimizer)
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

    // --- Table-scoped variable metadata ---

    //! Entity scope info for each source table with table-scoped variables.
    //! Empty if all variables are row-scoped (default behavior).
    vector<EntityScopeInfo> entity_scopes;

    //! Per-variable scope assignment: INVALID_INDEX = row-scoped (default),
    //! otherwise index into entity_scopes.
    vector<idx_t> variable_entity_scope;

public:
    // --- Implement virtual functions ---

    // The output columns are the child's columns plus the new decide variables
    vector<ColumnBinding> GetColumnBindings() override;

    // Resolve the output types
    void ResolveTypes() override;

    string GetName() const override;
    InsertionOrderPreservingMap<string> ParamsToString() const override;

    void Serialize(Serializer &serializer) const override;
    static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);
    
protected:
    // The table indices that this operator produces
    vector<idx_t> GetTableIndex() const override;

private:
    //! Recursively collect individual constraints from the AND-tree expression,
    //! unwrapping WHEN/PER wrappers for display
    static void CollectConstraintStrings(const Expression &expr, vector<string> &out);
};

} // namespace duckdb