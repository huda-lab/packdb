#include "duckdb/packdb/ilp_solver.hpp"
#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/packdb/gurobi/gurobi_solver.hpp"
#include "duckdb/packdb/naive/deterministic_naive.hpp"
#include "duckdb/common/exception.hpp"

#include <cstdlib>
#include <string>

namespace duckdb {

vector<double> SolveModel(const SolverInput &input) {
    SolverModel model = SolverModel::Build(input);

    // Test-only override: PACKDB_FORCE_SOLVER=highs|gurobi pins the backend.
    // Used by the DECIDE test suite (see test/decide/conftest.py fixtures
    // packdb_cli_highs / packdb_cli_gurobi) to exercise both backends on a
    // single host. Unknown values fall through to default auto-selection.
    if (const char *force = std::getenv("PACKDB_FORCE_SOLVER")) {
        std::string choice(force);
        if (choice == "highs" || choice == "HIGHS") {
            return DeterministicNaive::Solve(model);
        }
        if (choice == "gurobi" || choice == "GUROBI") {
            if (!GurobiSolver::IsAvailable()) {
                throw InvalidInputException(
                    "PACKDB_FORCE_SOLVER=gurobi but Gurobi is not available on this host");
            }
            return GurobiSolver::Solve(model);
        }
    }

    if (GurobiSolver::IsAvailable()) {
        return GurobiSolver::Solve(model);
    }
    return DeterministicNaive::Solve(model);
}

} // namespace duckdb
