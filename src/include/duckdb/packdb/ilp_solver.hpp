//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/packdb/ilp_solver.hpp
//
// Single entry point for optimization solving. Builds a SolverModel from
// SolverInput, selects the best available backend (Gurobi > HiGHS), and
// returns the solution vector. Supports LP, MILP, and convex QP/MIQP.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/packdb/solver_input.hpp"

namespace duckdb {

//! Builds a SolverModel from the given SolverInput, selects the best available
//! solver backend (Gurobi if licensed, otherwise HiGHS), solves, and returns
//! the solution vector of size (num_rows * num_decide_vars).
vector<double> SolveModel(const SolverInput &input);

} // namespace duckdb
