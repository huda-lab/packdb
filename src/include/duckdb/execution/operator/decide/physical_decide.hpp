#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

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
};

} // namespace duckdb