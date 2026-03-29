#include "duckdb/packdb/ilp_solver.hpp"
#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/packdb/gurobi/gurobi_solver.hpp"
#include "duckdb/packdb/naive/deterministic_naive.hpp"

namespace duckdb {

vector<double> SolveModel(const SolverInput &input) {
    SolverModel model = SolverModel::Build(input);

    if (GurobiSolver::IsAvailable()) {
        return GurobiSolver::Solve(model);
    }
    return DeterministicNaive::Solve(model);
}

} // namespace duckdb
