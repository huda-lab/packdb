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

    // Number of auxiliary variables (e.g. from ABS linearization) at the end of decide_variables
    idx_t num_auxiliary_vars = 0;

    // Links from COUNT indicator variables to their original variables (indicator_idx -> original_idx)
    vector<pair<idx_t, idx_t>> count_indicator_links;

    // Indices of auxiliary indicator variables for not-equal (<>) constraints
    vector<idx_t> ne_indicator_indices;

    // Links from MIN/MAX indicator variables: (agg_name "min"/"max", indicator_idx)
    vector<pair<string, idx_t>> minmax_indicator_links;

    // If objective uses MIN/MAX aggregate: "min" or "max", empty if SUM/AVG
    string minmax_objective_type;

public:
    // --- Implement virtual functions ---

    // The output columns are the child's columns plus the new decide variables
    vector<ColumnBinding> GetColumnBindings() override;

    // Resolve the output types
    void ResolveTypes() override;

    void Serialize(Serializer &serializer) const override;
    static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);
    
protected:
    // The table indices that this operator produces
    vector<idx_t> GetTableIndex() const override;
};

} // namespace duckdb