//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/packdb/ilp_model.hpp
//
// Solver-agnostic ILP model representation. Built once from SolverInput,
// consumed by any solver backend (HiGHS, Gurobi, etc.).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/packdb/naive/deterministic_naive.hpp"

namespace duckdb {

//! A single linear constraint: sum(coefficients[i] * x[indices[i]]) <sense> rhs
struct ILPConstraint {
    vector<int> indices;       //!< Variable indices into the flattened variable array
    vector<double> coefficients; //!< Coefficient for each variable
    char sense;                //!< '<' (<=), '>' (>=), '=' (==)
    double rhs;                //!< Right-hand side value
};

//! Solver-agnostic ILP model, ready for any backend to consume
struct ILPModel {
    idx_t num_vars;            //!< Total number of variables (num_rows * num_decide_vars)

    //! Per-variable configuration (size = num_vars)
    vector<double> col_lower;  //!< Lower bounds
    vector<double> col_upper;  //!< Upper bounds
    vector<bool> is_integer;   //!< True if integer (all DECIDE vars are integer)
    vector<bool> is_binary;    //!< True if binary (0/1), subset of integer

    //! Objective function
    vector<double> obj_coeffs; //!< Coefficient per variable
    bool maximize;             //!< True = maximize, false = minimize

    //! Constraints
    vector<ILPConstraint> constraints;

    //! Build an ILPModel from a SolverInput (the shared model-building logic)
    static ILPModel Build(const SolverInput &input);
};

} // namespace duckdb
