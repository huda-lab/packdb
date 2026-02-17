//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/packdb/gurobi/gurobi_solver.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/packdb/naive/deterministic_naive.hpp"

namespace duckdb {

class GurobiSolver {
public:
    //! Check if Gurobi is available at runtime (library linked + valid license)
    static bool IsAvailable();

    //! Solves the optimization problem using Gurobi and returns the solution vector
    //! The returned vector has size num_rows * num_decide_vars (same contract as DeterministicNaive::Solve)
    static vector<double> Solve(const SolverInput &input);
};

} // namespace duckdb
