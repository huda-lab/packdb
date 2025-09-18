// src/planner/operator/logical_decide.cpp
#include "duckdb/planner/operator/logical_decide.hpp"

#include "duckdb/packdb/utility/debug.hpp"

namespace duckdb {

LogicalDecide::LogicalDecide(idx_t decide_index, vector<unique_ptr<Expression>> decide_variables,
                             unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                             unique_ptr<Expression> decide_objective)
    : LogicalOperator(LogicalOperatorType::LOGICAL_DECIDE), decide_index(decide_index),
      decide_variables(std::move(decide_variables)), decide_constraints(std::move(decide_constraints)),
      decide_sense(decide_sense), decide_objective(std::move(decide_objective)) {
}

LogicalDecide::LogicalDecide() : LogicalOperator(LogicalOperatorType::LOGICAL_DECIDE) {
}

vector<ColumnBinding> LogicalDecide::GetColumnBindings() {
    // Return all child columns plus the new decide variables
    auto result = children[0]->GetColumnBindings();
    
    // Add the new columns produced by this operator (the decide variables)
    for (idx_t i = 0; i < decide_variables.size(); i++) {
        result.emplace_back(decide_index, i);
    }
    return result;
}

void LogicalDecide::ResolveTypes() {
    // Get all child types
    types = children[0]->types;
    
    // Add the types of the new decide variables
    for (const auto& var : decide_variables) {
        types.push_back(var->return_type);
    }
}

vector<idx_t> LogicalDecide::GetTableIndex() const {
	return vector<idx_t> {decide_index};
}

} // namespace duckdb