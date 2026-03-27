#include "duckdb/packdb/ilp_solver.hpp"
#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/packdb/gurobi/gurobi_solver.hpp"
#include "duckdb/packdb/naive/deterministic_naive.hpp"

#include <cstdio>

namespace duckdb {

vector<double> SolveILP(const SolverInput &input) {
    ILPModel model = ILPModel::Build(input);

    if (GurobiSolver::IsAvailable()) {
        return GurobiSolver::Solve(model);
    }
    fprintf(stderr, "Warning: Gurobi unavailable, falling back to HiGHS solver.\n");
    return DeterministicNaive::Solve(model);
}

} // namespace duckdb
