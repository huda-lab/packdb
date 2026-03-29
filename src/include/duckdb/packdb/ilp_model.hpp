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
    idx_t num_vars;            //!< Total number of variables (num_rows * num_decide_vars)

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
