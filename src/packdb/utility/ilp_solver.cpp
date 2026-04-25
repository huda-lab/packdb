#include "duckdb/packdb/ilp_solver.hpp"
#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/packdb/gurobi/gurobi_solver.hpp"
#include "duckdb/packdb/naive/deterministic_naive.hpp"
#include "duckdb/common/exception.hpp"

#include <cstdlib>
#include <string>

namespace duckdb {

namespace {

enum class SolverBackend { Gurobi, HiGHS };

struct ModelCapabilityNeeds {
    bool quadratic_constraints = false;
    bool nonconvex_objective = false;   // includes any bilinear-objective term
    bool miqp = false;                  // QP objective + any integer variable
    bool requires_gurobi() const {
        return quadratic_constraints || nonconvex_objective || miqp;
    }
};

// Inspect SolverInput cheaply (no Build) to decide whether HiGHS can solve it.
// Mirrors the rejection logic in DeterministicNaive::Solve and the convexity
// formula in SolverModel::Build, so we can reject incompatible backend pins
// before paying for the full Q/QC outer-product expansion in Build().
ModelCapabilityNeeds InspectModelCapabilities(const SolverInput &input) {
    ModelCapabilityNeeds needs;

    for (auto &c : input.constraints) {
        if (!c.bilinear_terms.empty() || c.has_quadratic) {
            needs.quadratic_constraints = true;
            break;
        }
    }

    bool has_qp = input.has_quadratic_objective;
    bool has_bilinear_obj = !input.bilinear_objective_terms.empty();

    if (has_qp) {
        // Mirror ilp_model_builder.cpp: nonconvex iff (sign>0) == is_max.
        bool is_max = (input.sense == DecideSense::MAXIMIZE);
        if ((input.quadratic_sign > 0.0) == is_max) {
            needs.nonconvex_objective = true;
        }
    }
    if (has_bilinear_obj) {
        // Bilinear (off-diagonal Q) is always indefinite => non-convex.
        needs.nonconvex_objective = true;
    }

    if (has_qp || has_bilinear_obj) {
        auto is_real = [](const LogicalType &t) {
            return t == LogicalType::DOUBLE || t == LogicalType::FLOAT;
        };
        bool any_integer = false;
        for (auto &t : input.variable_types) {
            if (!is_real(t)) { any_integer = true; break; }
        }
        if (!any_integer) {
            for (auto &t : input.global_variable_types) {
                if (!is_real(t)) { any_integer = true; break; }
            }
        }
        if (any_integer) {
            needs.miqp = true;
        }
    }

    return needs;
}

SolverBackend SelectBackend() {
    // Test-only override: PACKDB_FORCE_SOLVER=highs|gurobi pins the backend.
    // Used by the DECIDE test suite (see test/decide/conftest.py fixtures
    // packdb_cli_highs / packdb_cli_gurobi) to exercise both backends on a
    // single host. Unknown values fall through to default auto-selection.
    if (const char *force = std::getenv("PACKDB_FORCE_SOLVER")) {
        std::string choice(force);
        if (choice == "highs" || choice == "HIGHS") {
            return SolverBackend::HiGHS;
        }
        if (choice == "gurobi" || choice == "GUROBI") {
            if (!GurobiSolver::IsAvailable()) {
                throw InvalidInputException(
                    "PACKDB_FORCE_SOLVER=gurobi but Gurobi is not available on this host");
            }
            return SolverBackend::Gurobi;
        }
    }
    return GurobiSolver::IsAvailable() ? SolverBackend::Gurobi : SolverBackend::HiGHS;
}

} // namespace

vector<double> SolveModel(SolverInput &input, const VarIndexer &indexer) {
    // 1. Inspect SolverInput for backend-incompatible features.
    ModelCapabilityNeeds needs = InspectModelCapabilities(input);

    // 2. Pick backend. PACKDB_FORCE_SOLVER may pin HiGHS even on Gurobi-capable
    //    hosts, so we still need the rejection check below.
    SolverBackend backend = SelectBackend();

    // 3. Reject incompatible HiGHS path before Build(), saving the Q/QC build
    //    cost. The exact error texts mirror the in-Solve checks in
    //    DeterministicNaive::Solve so user-facing messages do not change.
    if (backend == SolverBackend::HiGHS && needs.requires_gurobi()) {
        if (needs.quadratic_constraints) {
            throw InvalidInputException(
                "Quadratic/bilinear constraints require Gurobi. "
                "HiGHS does not support quadratic constraints (QCQP). "
                "Either install Gurobi, or linearize the constraints.");
        }
        if (needs.nonconvex_objective) {
            throw InvalidInputException(
                "Non-convex quadratic objectives require Gurobi. "
                "HiGHS only supports convex quadratic programs "
                "(MINIMIZE with positive-semidefinite Q, or MAXIMIZE with negative-semidefinite Q). "
                "Either install Gurobi, or reformulate the objective.");
        }
        // remaining case: MIQP
        throw InvalidInputException(
            "Quadratic objectives with integer/boolean variables (MIQP) require Gurobi. "
            "HiGHS only supports continuous quadratic programs (QP). "
            "Either install Gurobi, or change all DECIDE variables to IS REAL.");
    }

    // 4. Build and solve.
    SolverModel model = SolverModel::Build(input, indexer);
    if (backend == SolverBackend::Gurobi) {
        return GurobiSolver::Solve(model);
    }
    return DeterministicNaive::Solve(model);
}

} // namespace duckdb
