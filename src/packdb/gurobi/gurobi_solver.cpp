#include "duckdb/packdb/gurobi/gurobi_solver.hpp"
#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/common/exception.hpp"
#include "gurobi_loader.hpp"

#include <cmath>

namespace duckdb {

//! RAII wrapper for Gurobi C resources (uses function pointers from loader)
struct GurobiGuard {
    void *model = nullptr;
    void *env = nullptr;
    ~GurobiGuard() {
        auto &api = GurobiLoader::API();
        if (model) {
            api.freemodel(model);
        }
        if (env) {
            api.freeenv(env);
        }
    }
};

bool GurobiSolver::IsAvailable() {
    static bool available = []() {
        if (!GurobiLoader::Load()) {
            return false;
        }
        // Trial: can we actually create and start an environment? (license check)
        auto &api = GurobiLoader::API();
        void *env = nullptr;
        bool ok = false;
        if (api.emptyenv_internal(&env, api.version_major, api.version_minor, api.version_tech) == 0 && env) {
            api.setintparam(env, "OutputFlag", 0);
            ok = (api.startenv(env) == 0);
        }
        if (env) {
            api.freeenv(env);
        }
        return ok;
    }();
    return available;
}

vector<double> GurobiSolver::Solve(const ILPModel &ilp) {
    auto &api = GurobiLoader::API();
    idx_t total_vars = ilp.num_vars;

    //===--------------------------------------------------------------------===//
    // 1. Create Gurobi environment
    //===--------------------------------------------------------------------===//

    GurobiGuard guard;
    int error = api.emptyenv_internal(&guard.env, api.version_major, api.version_minor, api.version_tech);
    if (error || !guard.env) {
        throw InternalException("Failed to create Gurobi environment (error %d). "
                                "Check that GUROBI_HOME is set and license is valid.",
                                error);
    }
    api.setintparam(guard.env, "OutputFlag", 0);
    error = api.startenv(guard.env);
    if (error) {
        throw InternalException("Failed to start Gurobi environment (error %d). "
                                "Check that GUROBI_HOME is set and license is valid.",
                                error);
    }

    //===--------------------------------------------------------------------===//
    // 2. Create model with variables
    //===--------------------------------------------------------------------===//

    vector<char> var_types(total_vars);
    for (idx_t i = 0; i < total_vars; i++) {
        if (ilp.is_binary[i]) {
            var_types[i] = GRB_BINARY;
        } else if (ilp.is_integer[i]) {
            var_types[i] = GRB_INTEGER;
        } else {
            var_types[i] = GRB_CONTINUOUS;
        }
    }

    error = api.newmodel(guard.env, &guard.model, "packdb_decide",
                         (int)total_vars,
                         const_cast<double *>(ilp.obj_coeffs.data()),
                         const_cast<double *>(ilp.col_lower.data()),
                         const_cast<double *>(ilp.col_upper.data()),
                         var_types.data(), nullptr);
    if (error) {
        throw InternalException("Failed to create Gurobi model: %s",
                                api.geterrormsg(guard.env));
    }

    int grb_sense = ilp.maximize ? GRB_MAXIMIZE : GRB_MINIMIZE;
    error = api.setintattr(guard.model, GRB_INT_ATTR_MODELSENSE, grb_sense);
    if (error) {
        throw InternalException("Failed to set Gurobi model sense: %s",
                                api.geterrormsg(guard.env));
    }

    //===--------------------------------------------------------------------===//
    // 3. Add constraints
    //===--------------------------------------------------------------------===//

    for (auto &constr : ilp.constraints) {
        error = api.addconstr(guard.model, (int)constr.indices.size(),
                             const_cast<int *>(constr.indices.data()),
                             const_cast<double *>(constr.coefficients.data()),
                             constr.sense, constr.rhs, nullptr);
        if (error) {
            throw InternalException("Failed to add constraint to Gurobi: %s",
                                    api.geterrormsg(guard.env));
        }
    }

    //===--------------------------------------------------------------------===//
    // 4. Solve
    //===--------------------------------------------------------------------===//

    error = api.optimize(guard.model);
    if (error) {
        throw InternalException("Gurobi optimization call failed: %s",
                                api.geterrormsg(guard.env));
    }

    //===--------------------------------------------------------------------===//
    // 5. Check status
    //===--------------------------------------------------------------------===//

    int status;
    error = api.getintattr(guard.model, GRB_INT_ATTR_STATUS, &status);
    if (error) {
        throw InternalException("Failed to get Gurobi status: %s",
                                api.geterrormsg(guard.env));
    }

    if (status != GRB_OPTIMAL) {
        if (status == GRB_INFEASIBLE) {
            throw InvalidInputException(
                "DECIDE optimization is infeasible: No valid solution exists that satisfies all constraints.\n\n"
                "This means the SUCH THAT conditions cannot all be met simultaneously.\n\n"
                "Common causes:\n"
                "  • Contradictory bounds (e.g., x >= 10 AND x <= 5)\n"
                "  • SUM constraints impossible to satisfy with available data\n"
                "  • Variable types too restrictive (BOOLEAN when INTEGER needed)\n\n"
                "Suggestion: Try relaxing constraints or verify input data.");
        } else if (status == GRB_UNBOUNDED || status == GRB_INF_OR_UNBD) {
            throw InvalidInputException(
                "DECIDE optimization is unbounded: The objective can grow infinitely.\n\n"
                "This means the MAXIMIZE/MINIMIZE goal has no finite optimal value.\n"
                "You must add constraints to bound the decision variables.\n\n"
                "Examples:\n"
                "  • Add upper bounds: SUCH THAT x <= 100\n"
                "  • Add budget limits: SUCH THAT SUM(x * cost) <= budget\n"
                "  • Use BOOLEAN instead of INTEGER for selection problems");
        } else if (status == GRB_TIME_LIMIT) {
            throw InvalidInputException(
                "DECIDE optimization exceeded time limit.\n"
                "The problem may be too complex to solve in reasonable time.\n"
                "Try simplifying constraints or reducing data size.");
        } else if (status == GRB_ITERATION_LIMIT) {
            throw InvalidInputException(
                "DECIDE optimization exceeded iteration limit.\n"
                "The problem may be too complex. Try simplifying constraints.");
        } else {
            throw InvalidInputException(
                "DECIDE optimization failed with Gurobi status %d.\n"
                "The optimization could not find a solution.\n"
                "This may indicate a problem with the constraints or objective.",
                status);
        }
    }

    //===--------------------------------------------------------------------===//
    // 6. Extract solution
    //===--------------------------------------------------------------------===//

    vector<double> result(total_vars);
    error = api.getdblattrarray(guard.model, GRB_DBL_ATTR_X, 0, (int)total_vars, result.data());
    if (error) {
        throw InternalException("Failed to extract Gurobi solution: %s",
                                api.geterrormsg(guard.env));
    }

    for (idx_t i = 0; i < total_vars; i++) {
        if (!std::isfinite(result[i])) {
            throw InternalException(
                "Gurobi returned invalid solution value (NaN or Infinity) for variable %llu", i);
        }
    }

    return result;
}

} // namespace duckdb
