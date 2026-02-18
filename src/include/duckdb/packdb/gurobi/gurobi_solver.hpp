//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/packdb/gurobi/gurobi_solver.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

struct ILPModel;

class GurobiSolver {
public:
    //! Check if Gurobi is available at runtime (library linked + valid license)
    static bool IsAvailable();

    //! Solves the optimization problem using Gurobi.
    //! Takes a solver-agnostic ILPModel (already built from SolverInput).
    //! Returns the solution vector (size = num_vars).
    static vector<double> Solve(const ILPModel &model);
};

} // namespace duckdb
