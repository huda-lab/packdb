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
    // Return only the required child columns plus the new decide variables
    auto result = required_child_columns;
    
    // Add the new columns produced by this operator
    for (idx_t i = 0; i < decide_variables.size(); i++) {
        result.emplace_back(decide_index, i);
    }
    return result;
}

void LogicalDecide::ResolveTypes() {
    // Get types for only the required child columns
    auto child_bindings = children[0]->GetColumnBindings();
    auto child_types = children[0]->types;
    
    types.clear();
    
    // Add types for the required child columns only
    for (const auto& required_binding : required_child_columns) {
        // Find the index of this binding in the child's bindings
        for (idx_t i = 0; i < child_bindings.size(); i++) {
            if (child_bindings[i] == required_binding) {
                types.push_back(child_types[i]);
                break;
            }
        }
    }
    
    // Add the types of the new decide variables
    for (const auto& var : decide_variables) {
        types.push_back(var->return_type);
    }
}

vector<idx_t> LogicalDecide::GetTableIndex() const {
	return vector<idx_t> {decide_index};
}

} // namespace duckdb