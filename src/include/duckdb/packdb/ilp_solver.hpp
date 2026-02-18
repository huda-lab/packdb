//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/packdb/ilp_solver.hpp
//
// Single entry point for ILP solving. Builds the ILPModel from a SolverInput,
// selects the best available backend (Gurobi > HiGHS), and returns the
// solution vector.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/packdb/solver_input.hpp"

namespace duckdb {

//! Builds an ILPModel from the given SolverInput, selects the best available
//! solver backend (Gurobi if licensed, otherwise HiGHS), solves, and returns
//! the solution vector of size (num_rows * num_decide_vars).
vector<double> SolveILP(const SolverInput &input);

} // namespace duckdb
