//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/packdb/solver_input.hpp
//
// Solver-agnostic input structs for the DECIDE optimization formulation.
// These are built by physical_decide.cpp and consumed by the solver facade.
// Supports LP, MILP, and convex QP/MIQP objectives.
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
    bool was_avg_rewrite = false;              // True if originally AVG (RHS scaled by row count)
    idx_t minmax_indicator_idx = DConstants::INVALID_INDEX;  // Indicator var idx for hard MIN/MAX
    string minmax_agg_type;                    // "min" or "max" (empty if not minmax)
    idx_t ne_indicator_idx = DConstants::INVALID_INDEX;      // Indicator var idx for not-equal

    //! Unified WHEN+PER row→group mapping
    //! Empty = all rows in one implicit group (fast path: no WHEN, no PER)
    //! DConstants::INVALID_INDEX = row excluded (WHEN filter or NULL PER value)
    //! 0..K-1 = group assignment
    vector<idx_t> row_group_ids;
    idx_t num_groups = 0;                     // 0 = ungrouped, >0 = number of distinct groups
};

//! Maps result rows to unique entities in a source table.
//! Used for table-scoped decision variables where one variable value
//! is shared across all result rows from the same base table entity.
struct EntityMapping {
    idx_t num_entities = 0;          //! Number of distinct entities in this table
    vector<idx_t> row_to_entity;     //! [row_idx] -> entity_id (0..num_entities-1)
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

    // Linear objective
    vector<vector<double>> objective_coefficients; // [term_idx][row_idx]
    vector<idx_t> objective_variable_indices;      // [term_idx]
    DecideSense sense;

    // Quadratic objective: inner linear expression of SUM(POWER(expr, 2)).
    // When has_quadratic_objective is true, the objective includes a convex
    // quadratic component. The inner expression coefficients are stored per-term
    // and per-row, just like the linear objective. The model builder expands
    // these into the Q matrix via outer products (Q = A^T A, always PSD).
    bool has_quadratic_objective = false;
    vector<vector<double>> quadratic_inner_coefficients; // [term_idx][row_idx]
    vector<idx_t> quadratic_inner_variable_indices;      // [term_idx]

    // Objective PER grouping (mirrors constraint row_group_ids pattern)
    vector<idx_t> objective_row_group_ids;  // per-row group assignment
    idx_t objective_num_groups = 0;          // 0 = ungrouped

    // Global auxiliary variables (exist once, not replicated per row)
    // Appended after the per-row grid at indices num_rows * num_decide_vars + i
    idx_t num_global_vars = 0;
    vector<LogicalType> global_variable_types;
    vector<double> global_lower_bounds;
    vector<double> global_upper_bounds;
    vector<double> global_obj_coeffs;  // Objective coefficients for global vars

    // Raw ILP constraints involving global variables (indices are absolute into the
    // flattened variable array including global vars)
    struct RawConstraint {
        vector<int> indices;
        vector<double> coefficients;
        char sense;     // '<' (<=), '>' (>=), '=' (==)
        double rhs;
    };
    vector<RawConstraint> global_constraints;

    // --- Table-scoped variable support ---

    //! Entity mappings: one per EntityScopeInfo (source table with scoped vars)
    vector<EntityMapping> entity_mappings;

    //! Per-variable scope: INVALID_INDEX = row-scoped (default),
    //! otherwise index into entity_mappings
    vector<idx_t> variable_entity_scope;
};

} // namespace duckdb
