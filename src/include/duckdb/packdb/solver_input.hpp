//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/packdb/solver_input.hpp
//
// Solver-agnostic input structs for the DECIDE ILP formulation.
// These are built by physical_decide.cpp and consumed by the solver facade.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! Represents an evaluated constraint ready for the solver
struct EvaluatedConstraint {
    vector<idx_t> variable_indices;           // Which variable for each term
    vector<vector<double>> row_coefficients;  // [term_idx][row_idx] = coefficient value
    vector<double> rhs_values;                // [row_idx] = RHS value
    ExpressionType comparison_type;
    bool lhs_is_aggregate = false;            // True if original LHS was an aggregate (e.g., SUM(...))

    //! Unified WHEN+PER row→group mapping
    //! Empty = all rows in one implicit group (fast path: no WHEN, no PER)
    //! DConstants::INVALID_INDEX = row excluded (WHEN filter or NULL PER value)
    //! 0..K-1 = group assignment
    vector<idx_t> row_group_ids;
    idx_t num_groups = 0;                     // 0 = ungrouped, >0 = number of distinct groups
};

//! Input for the deterministic solver
struct SolverInput {
    idx_t num_rows;
    idx_t num_decide_vars;

    // Per-variable configuration (size = num_decide_vars)
    vector<LogicalType> variable_types;
    vector<double> lower_bounds;
    vector<double> upper_bounds;

    // Constraints
    vector<EvaluatedConstraint> constraints;

    // Objective
    vector<vector<double>> objective_coefficients; // [term_idx][row_idx]
    vector<idx_t> objective_variable_indices;      // [term_idx]
    DecideSense sense;
};

} // namespace duckdb
