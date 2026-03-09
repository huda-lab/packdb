#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"

namespace duckdb {

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalDecide &op) {
    D_ASSERT(op.children.size() == 1);
    auto child_plan = CreatePlan(*op.children[0]);
    auto decide_op = make_uniq<PhysicalDecide>(
        op.types, op.estimated_cardinality, std::move(child_plan),
        op.decide_index, std::move(op.decide_variables),
        std::move(op.decide_constraints), op.decide_sense, std::move(op.decide_objective));
    decide_op->num_auxiliary_vars = op.num_auxiliary_vars;
    decide_op->count_indicator_links = std::move(op.count_indicator_links);
    decide_op->ne_indicator_indices = std::move(op.ne_indicator_indices);
    decide_op->minmax_indicator_links = std::move(op.minmax_indicator_links);
    decide_op->minmax_objective_type = std::move(op.minmax_objective_type);
    return std::move(decide_op);
}

} // namespace duckdb