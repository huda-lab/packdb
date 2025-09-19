#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include "duckdb/packdb/utility/debug.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

PhysicalDecide::PhysicalDecide(vector<LogicalType> types, idx_t estimated_cardinality, 
                    unique_ptr<PhysicalOperator> child, idx_t decide_index, 
                    vector<unique_ptr<Expression>> decide_variables,
                    unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                    unique_ptr<Expression> decide_objective)
    : PhysicalOperator(PhysicalOperatorType::DECIDE, std::move(types), estimated_cardinality)
    , decide_index(decide_index)
    , decide_variables(std::move(decide_variables))
    , decide_constraints(std::move(decide_constraints))
    , decide_sense(decide_sense)
    , decide_objective(std::move(decide_objective)) {
    children.push_back(std::move(child));
}

struct DeterministicConstraint {
    idx_t variable_index;
    DeterministicConstraintSense sense;
    const Expression& coef;
    const Expression& rhs;
    DeterministicConstraint(idx_t var_idx, DeterministicConstraintSense s,
    const Expression& c, const Expression& r)
            : variable_index(var_idx), sense(s), coef(c), rhs(r) {}
};

struct DeterministicObjective {
    idx_t variable_index;
    const Expression& coef;
    DeterministicObjective(idx_t var_idx, const Expression& c) : variable_index(var_idx), coef(c) {}
};

//===--------------------------------------------------------------------===//
// Sink (Collecting Data)
//===--------------------------------------------------------------------===//
class DecideGlobalSinkState : public GlobalSinkState {
public:
    explicit DecideGlobalSinkState(ClientContext &context, const PhysicalDecide &op)
        : data(context, op.children[0]->GetTypes()), op(op) {
        AnalyzeConstraint(op.decide_constraints);
        AnalyzeObjective(op.decide_objective);
    }

    int AnalyzeVariable(const unique_ptr<Expression>& expr_ptr) {
        auto &expr = *expr_ptr;
        switch(expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_COLUMN_REF: {
                auto &column_ref = expr.Cast<BoundColumnRefExpression>();
                for (idx_t i = 0; i < op.decide_variables.size(); i++) {
                    if (op.decide_variables[i]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                        auto &var_ref = op.decide_variables[i]->Cast<BoundColumnRefExpression>();
                        if (var_ref.binding.column_index == column_ref.binding.column_index) {
                            return i;
                        }
                    }
                }
                return -1;
            }
            default:
                return -1;
        }
    }

    pair<int, const unique_ptr<Expression>&> AnalyzeSumArgument(const unique_ptr<Expression>& expr_ptr) {
        auto &agg = expr_ptr->Cast<BoundAggregateExpression>();
        auto &func = agg.children.front()->Cast<BoundFunctionExpression>();
        int left_index = AnalyzeVariable(func.children.front());
        int right_index = AnalyzeVariable(func.children.back());
        if (left_index >= 0) return {left_index, func.children.back()};
        if (right_index >= 0) return {right_index, func.children.front()};
        return {-1, NULL};
    }

    void AnalyzeConstraint(const unique_ptr<Expression>& expr_ptr) {
        auto &expr = *expr_ptr;
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                for (idx_t i = 0; i < conj.children.size(); i++) {
                    AnalyzeConstraint(conj.children[i]);
                }
                break;
            }
            case ExpressionClass::BOUND_COMPARISON: {
                auto &comp = expr.Cast<BoundComparisonExpression>();
                if (comp.left->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
                    auto arg = AnalyzeSumArgument(comp.left);
                    assert(arg.first >= 0);
                    switch (comp.type) {
                        case ExpressionType::COMPARE_EQUAL: {
                            auto constraint = make_uniq<DeterministicConstraint>(arg.first, DeterministicConstraintSense::EQ, *arg.second, *comp.right);
                            cons.push_back(std::move(constraint));
                            break;
                        }
                        case ExpressionType::COMPARE_LESSTHANOREQUALTO: {
                            auto constraint = make_uniq<DeterministicConstraint>(arg.first, DeterministicConstraintSense::LTEQ, *arg.second, *comp.right);
                            cons.push_back(std::move(constraint));
                            break;
                        }
                        case ExpressionType::COMPARE_GREATERTHANOREQUALTO: {
                            auto constraint = make_uniq<DeterministicConstraint>(arg.first, DeterministicConstraintSense::GTEQ, *arg.second, *comp.right);
                            cons.push_back(std::move(constraint));
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
            default:
                break;
        }
    }

    void AnalyzeObjective(const unique_ptr<Expression>& expr_ptr) {
        if (expr_ptr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
            auto arg = AnalyzeSumArgument(expr_ptr);
            obj = make_uniq<DeterministicObjective>(arg.first, *arg.second);
        }
    }

    mutex lock;
    // This collection will hold all the data from the child operator
    ColumnDataCollection data;

    const PhysicalDecide &op;
    vector<unique_ptr<DeterministicConstraint>> cons;
    unique_ptr<DeterministicObjective> obj;

    // This will hold the solution from the ILP solver
    vector<int64_t> ilp_solution;
};

class DecideLocalSinkState : public LocalSinkState {
public:
    explicit DecideLocalSinkState(ClientContext &context, const PhysicalDecide &op)
        : data(context, op.children[0]->GetTypes()) {
        data.InitializeAppend(append_state);
    }

    // A local collection to buffer chunks before merging into the global state
    ColumnDataCollection data;
    ColumnDataAppendState append_state;
};

unique_ptr<GlobalSinkState> PhysicalDecide::GetGlobalSinkState(ClientContext &context) const {
    return make_uniq<DecideGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalDecide::GetLocalSinkState(ExecutionContext &context) const {
    return make_uniq<DecideLocalSinkState>(context.client, *this);
}

SinkResultType PhysicalDecide::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
    auto &lstate = input.local_state.Cast<DecideLocalSinkState>();
    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    ExpressionExecutor coef_executor (context.client);
    ExpressionExecutor rhs_executor (context.client);
    // for (const auto& con : gstate.cons) {
    //     coef_executor.AddExpression(con->coef);
    //     rhs_executor.AddExpression(con->rhs);
    // }
    // coef_executor.AddExpression(gstate.obj->coef);
    lstate.data.Append(lstate.append_state, chunk);
    return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalDecide::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    auto &lstate = input.local_state.Cast<DecideLocalSinkState>();

    lock_guard<mutex> guard(gstate.lock);
    gstate.data.Combine(lstate.data);

    return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalDecide::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    //
    // --- THIS IS WHERE YOU SOLVE THE ILP ---
    //
    // At this point, gstate.data contains all the input rows.
    // You can iterate over it to build your ILP model.

    // 1. Formulate the Integer Linear Program
    //    You would iterate through gstate.data, e.g.:
    //    for (auto &chunk : gstate.data.Chunks()) {
    //        // Extract data from chunk.data[...].GetValue(row_idx)
    //    } 

    // 2. Solve the ILP
    //    e.g., auto solution_vector = MyILPSolver.Solve(ilp_model);
    //
    // For this example, we'll just generate a dummy solution.
    // Let's say the solution is just the row number.
    
    gstate.ilp_solution.reserve(gstate.data.Count());
    for (idx_t i = 0; i < gstate.data.Count(); i++) {
        // Replace this with the actual solution for row 'i'
        gstate.ilp_solution.push_back(i % 100);
    }

    return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source (Producing Output)
//===--------------------------------------------------------------------===//
class DecideGlobalSourceState : public GlobalSourceState {
public:
    explicit DecideGlobalSourceState(const PhysicalDecide &op, DecideGlobalSinkState &sink) {
        sink.data.InitializeScan(scan_state);
    }

    ColumnDataScanState scan_state;

    idx_t MaxThreads() override {
        return 1; // For simplicity, we'll make the source single-threaded.
    }
};

unique_ptr<GlobalSourceState> PhysicalDecide::GetGlobalSourceState(ClientContext &context) const {
    auto &sink = sink_state->Cast<DecideGlobalSinkState>();
    return make_uniq<DecideGlobalSourceState>(*this, sink);
}

SourceResultType PhysicalDecide::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
    auto &gstate = sink_state->Cast<DecideGlobalSinkState>();
    auto &source_state = input.global_state.Cast<DecideGlobalSourceState>();

    // Scan the original buffered data
    gstate.data.Scan(source_state.scan_state, chunk);
    if (chunk.size() == 0) {
        return SourceResultType::FINISHED;
    }
    
    // types is the output columns
    // children[0]->GetTypes() is the input columns
    // deb(types, children[0]->GetTypes());

    for (idx_t i = 0; i < decide_variables.size(); i++) {
        // The new column is the next available column in the output chunk
        auto &output_vector = chunk.data[types.size() - decide_variables.size() + i];
        // D_ASSERT(output_vector.GetType().id() == LogicalTypeId::DOUBLE);

        // Set all values in this new column to a constant index
        if (i == 0) output_vector.Reference(Value::INTEGER(i+10));
        else output_vector.Reference(Value::DOUBLE(i+10));
        // For now set the values to NULL
        // output_vector.SetVectorType(VectorType::CONSTANT_VECTOR);
        // ConstantVector::SetNull(output_vector, true);
    }
    // The chunk's cardinality was already set by the Scan call.
    return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb