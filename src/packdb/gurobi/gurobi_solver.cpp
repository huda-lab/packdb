#include "duckdb/packdb/gurobi/gurobi_solver.hpp"
#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/common/exception.hpp"

#include <cmath>

#if PACKDB_HAS_GUROBI
#include "gurobi_c.h"
#endif

namespace duckdb {

#if PACKDB_HAS_GUROBI

//! RAII wrapper for Gurobi C resources
struct GurobiGuard {
    GRBmodel *model = nullptr;
    GRBenv *env = nullptr;
    ~GurobiGuard() {
        if (model) {
            GRBfreemodel(model);
        }
        if (env) {
            GRBfreeenv(env);
        }
    }
};

#endif // PACKDB_HAS_GUROBI

bool GurobiSolver::IsAvailable() {
#if PACKDB_HAS_GUROBI
    static bool available = []() {
        GRBenv *env = nullptr;
        int error = GRBloadenv(&env, nullptr);
        bool ok = (error == 0 && env != nullptr);
        if (env) {
            GRBfreeenv(env);
        }
        return ok;
    }();
    return available;
#else
    return false;
#endif
}

vector<double> GurobiSolver::Solve(const ILPModel &ilp) {
#if PACKDB_HAS_GUROBI
    idx_t total_vars = ilp.num_vars;

    //===--------------------------------------------------------------------===//
    // 1. Create Gurobi environment
    //===--------------------------------------------------------------------===//

    GurobiGuard guard;
    int error = GRBloadenv(&guard.env, nullptr);
    if (error) {
        throw InternalException("Failed to create Gurobi environment (error %d). "
                                "Check that GUROBI_HOME is set and license is valid.",
                                error);
    }

    GRBsetintparam(guard.env, "OutputFlag", 0);

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

    error = GRBnewmodel(guard.env, &guard.model, "packdb_decide",
                         (int)total_vars,
                         const_cast<double *>(ilp.obj_coeffs.data()),
                         const_cast<double *>(ilp.col_lower.data()),
                         const_cast<double *>(ilp.col_upper.data()),
                         var_types.data(), nullptr);
    if (error) {
        throw InternalException("Failed to create Gurobi model: %s",
                                GRBgeterrormsg(guard.env));
    }

    int grb_sense = ilp.maximize ? GRB_MAXIMIZE : GRB_MINIMIZE;
    error = GRBsetintattr(guard.model, GRB_INT_ATTR_MODELSENSE, grb_sense);
    if (error) {
        throw InternalException("Failed to set Gurobi model sense: %s",
                                GRBgeterrormsg(guard.env));
    }

    //===--------------------------------------------------------------------===//
    // 3. Add constraints
    //===--------------------------------------------------------------------===//

    for (auto &constr : ilp.constraints) {
        error = GRBaddconstr(guard.model, (int)constr.indices.size(),
                             const_cast<int *>(constr.indices.data()),
                             const_cast<double *>(constr.coefficients.data()),
                             constr.sense, constr.rhs, nullptr);
        if (error) {
            throw InternalException("Failed to add constraint to Gurobi: %s",
                                    GRBgeterrormsg(guard.env));
        }
    }

    //===--------------------------------------------------------------------===//
    // 4. Solve
    //===--------------------------------------------------------------------===//

    error = GRBoptimize(guard.model);
    if (error) {
        throw InternalException("Gurobi optimization call failed: %s",
                                GRBgeterrormsg(guard.env));
    }

    //===--------------------------------------------------------------------===//
    // 5. Check status
    //===--------------------------------------------------------------------===//

    int status;
    error = GRBgetintattr(guard.model, GRB_INT_ATTR_STATUS, &status);
    if (error) {
        throw InternalException("Failed to get Gurobi status: %s",
                                GRBgeterrormsg(guard.env));
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
    error = GRBgetdblattrarray(guard.model, GRB_DBL_ATTR_X, 0, (int)total_vars, result.data());
    if (error) {
        throw InternalException("Failed to extract Gurobi solution: %s",
                                GRBgeterrormsg(guard.env));
    }

    for (idx_t i = 0; i < total_vars; i++) {
        if (!std::isfinite(result[i])) {
            throw InternalException(
                "Gurobi returned invalid solution value (NaN or Infinity) for variable %llu", i);
        }
    }

    return result;

#else
    (void)ilp;
    throw InternalException("Gurobi solver requested but PackDB was built without Gurobi support. "
                            "Set GUROBI_HOME and rebuild to enable Gurobi.");
#endif
}

} // namespace duckdb
