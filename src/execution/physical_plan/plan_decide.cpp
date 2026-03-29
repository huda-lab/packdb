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
    decide_op->flat_objective_agg = op.flat_objective_agg;
    decide_op->flat_objective_is_easy = op.flat_objective_is_easy;
    decide_op->per_inner_agg = op.per_inner_agg;
    decide_op->per_outer_agg = op.per_outer_agg;
    decide_op->per_inner_is_easy = op.per_inner_is_easy;
    decide_op->per_outer_is_easy = op.per_outer_is_easy;
    decide_op->per_inner_was_avg = op.per_inner_was_avg;
    return std::move(decide_op);
}

} // namespace duckdb