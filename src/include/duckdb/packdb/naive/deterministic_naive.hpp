#pragma once

#include "duckdb/packdb/solver_input.hpp"

namespace duckdb {

struct ILPModel;

class DeterministicNaive {
public:
    //! Solves the optimization problem using HiGHS.
    //! Takes a solver-agnostic ILPModel (already built from SolverInput).
    //! Returns the solution vector (size = num_rows * num_decide_vars).
    static vector<double> Solve(const ILPModel &model);
};

} // namespace duckdb
