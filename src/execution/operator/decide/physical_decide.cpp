#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

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

//===--------------------------------------------------------------------===//
// Sink (Collecting Data)
//===--------------------------------------------------------------------===//
class DecideGlobalSinkState : public GlobalSinkState {
public:
    explicit DecideGlobalSinkState(ClientContext &context, const PhysicalDecide &op)
        : data(context, op.children[0]->GetTypes()) {
        // Here you would initialize your ILP solver's global state if needed
    }

    mutex lock;
    // This collection will hold all the data from the child operator
    ColumnDataCollection data;
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
    
    // gstate.ilp_solution.reserve(gstate.data.Count());
    // for (idx_t i = 0; i < gstate.data.Count(); i++) {
    //     // Replace this with the actual solution for row 'i'
    //     gstate.ilp_solution.push_back(i % 100);
    // }

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
    
    idx_t child_column_count = children[0]->GetTypes().size();
    idx_t new_column_count = types.size() - child_column_count;

    for (idx_t i = 0; i < new_column_count; i++) {
        // The new column is the next available column in the output chunk
        auto &output_vector = chunk.data[child_column_count + i];
        D_ASSERT(output_vector.GetType().id() == LogicalTypeId::DOUBLE);

        // Set all values in this new column to a constant 0.0
        output_vector.Reference(Value::DOUBLE(0.0));
    }
    // The chunk's cardinality was already set by the Scan call.
    return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb