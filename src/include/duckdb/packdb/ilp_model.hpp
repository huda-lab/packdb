//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/packdb/ilp_model.hpp
//
// Solver-agnostic optimization model representation. Built once from
// SolverInput, consumed by any solver backend (HiGHS, Gurobi, etc.).
// Supports LP, MILP, and convex QP/MIQP.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/packdb/solver_input.hpp"

namespace duckdb {

//! Maps (decide_var_idx, row) pairs to flat solver variable indices.
//! Supports mixed row-scoped and entity-scoped variables with a three-block layout:
//!   Block 1: row-scoped vars  (num_rows * num_row_vars entries)
//!   Block 2: entity-scoped vars (sum of num_entities per scope)
//!   Block 3: global auxiliary vars
struct VarIndexer {
    idx_t num_rows = 0;
    idx_t num_row_vars = 0;          //!< Count of row-scoped decide variables
    idx_t entity_block_start = 0;    //!< First index of entity block
    idx_t global_block_start = 0;    //!< First index of global block
    idx_t total_vars = 0;

    //! Per decide-variable: true if entity-scoped
    vector<bool> is_entity_scoped;

    //! For row-scoped vars: offset within the row block (var_idx → position in row)
    vector<idx_t> row_var_offset;

    //! For entity-scoped vars: base offset in entity block
    vector<idx_t> entity_var_base;

    //! For entity-scoped vars: index into entity_mappings source
    vector<idx_t> var_entity_mapping_idx;

    //! Pointer to entity mappings (not owned — caller ensures lifetime)
    const vector<EntityMapping> *entity_mappings_ref = nullptr;
    //! Owned copy of entity mappings (used when VarIndexer must outlive its source)
    vector<EntityMapping> entity_mappings_owned;

    //! Get the flat solver variable index for a given decide variable at a given row
    inline idx_t Get(idx_t var_idx, idx_t row) const {
        if (!is_entity_scoped[var_idx]) {
            return row * num_row_vars + row_var_offset[var_idx];
        }
        auto &mappings = entity_mappings_ref ? *entity_mappings_ref : entity_mappings_owned;
        auto &mapping = mappings[var_entity_mapping_idx[var_idx]];
        idx_t entity_id = mapping.row_to_entity[row];
        return entity_var_base[var_idx] + entity_id;
    }

    //! Get the number of instances (copies) for a given decide variable
    inline idx_t NumInstances(idx_t var_idx) const {
        if (!is_entity_scoped[var_idx]) {
            return num_rows;
        }
        auto &mappings = entity_mappings_ref ? *entity_mappings_ref : entity_mappings_owned;
        return mappings[var_entity_mapping_idx[var_idx]].num_entities;
    }

    //! Build a VarIndexer that OWNS a copy of entity_mappings.
    //! Safe to use after the SolverInput is destroyed (e.g., stored on gstate for readback).
    static VarIndexer Build(const SolverInput &input);

    //! Build a VarIndexer that REFERENCES entity_mappings without copying.
    //! Caller must ensure the SolverInput outlives this VarIndexer.
    //! Used for temporary indexers (pre_indexer, model builder).
    static VarIndexer BuildRef(const SolverInput &input);
};

//! A single linear constraint: sum(coefficients[i] * x[indices[i]]) <sense> rhs
struct ModelConstraint {
    vector<int> indices;       //!< Variable indices into the flattened variable array
    vector<double> coefficients; //!< Coefficient for each variable
    char sense;                //!< '<' (<=), '>' (>=), '=' (==)
    double rhs;                //!< Right-hand side value
};

//! Solver-agnostic optimization model, ready for any backend to consume.
//! Supports linear objectives (LP/MILP) and convex quadratic objectives (QP/MIQP).
struct SolverModel {
    idx_t num_vars;            //!< Total number of variables across all three blocks (row-scoped + entity-scoped + global auxiliary)

    //! Per-variable configuration (size = num_vars)
    vector<double> col_lower;  //!< Lower bounds
    vector<double> col_upper;  //!< Upper bounds
    vector<bool> is_integer;   //!< True for INTEGER/BOOLEAN vars, false for REAL (continuous)
    vector<bool> is_binary;    //!< True if binary (0/1), subset of integer

    //! Linear objective: minimize/maximize c^T x
    vector<double> obj_coeffs; //!< Coefficient per variable (linear part)
    bool maximize;             //!< True = maximize, false = minimize

    //! Quadratic objective: (1/2) x^T Q x (added to linear part).
    //! Stored in COO (coordinate) format, lower triangle only.
    //! Empty when the objective is purely linear (LP/MILP).
    vector<int> q_rows;        //!< Row indices into Q
    vector<int> q_cols;        //!< Column indices into Q
    vector<double> q_vals;     //!< Values in Q
    bool has_quadratic_obj = false;

    //! Constraints (linear)
    vector<ModelConstraint> constraints;

    //! Build a SolverModel from a SolverInput (the shared model-building logic)
    static SolverModel Build(const SolverInput &input);
};

} // namespace duckdb
