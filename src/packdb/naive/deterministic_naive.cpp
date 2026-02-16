#include "duckdb/packdb/naive/deterministic_naive.hpp"
#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/common/exception.hpp"
#include "Highs.h"

#include <cmath>

namespace duckdb {

vector<double> DeterministicNaive::Solve(const SolverInput& input) {
    // Build solver-agnostic ILP model (shared logic)
    ILPModel model = ILPModel::Build(input);

    idx_t total_vars = model.num_vars;
    idx_t num_constraints = model.constraints.size();

    //===--------------------------------------------------------------------===//
    // 1. Create HiGHS model and set up variables
    //===--------------------------------------------------------------------===//

    Highs highs;
    highs.setOptionValue("log_to_console", false);

    // Convert ILPModel types to HiGHS types
    vector<HighsVarType> var_types(total_vars);
    for (idx_t i = 0; i < total_vars; i++) {
        var_types[i] = model.is_integer[i] ? HighsVarType::kInteger : HighsVarType::kContinuous;
    }

    ObjSense sense = model.maximize ? ObjSense::kMaximize : ObjSense::kMinimize;

    //===--------------------------------------------------------------------===//
    // 2. Convert constraints to HiGHS range format (row_lower, row_upper)
    //    and build COO constraint matrix
    //===--------------------------------------------------------------------===//

    vector<int> a_rows;
    vector<int> a_cols;
    vector<double> a_vals;
    vector<double> row_lower;
    vector<double> row_upper;

    idx_t constraint_idx = 0;
    for (auto &eval_const : input.constraints) {
        // Use original provenance: aggregate if and only if LHS was an aggregate (e.g., SUM(...))
        bool is_aggregate = eval_const.lhs_is_aggregate;
        bool has_mask = !eval_const.row_mask.empty();

        if (is_aggregate) {
            // AGGREGATE CONSTRAINT: SUM(x) <= 10 [WHEN condition]
            // Create ONE constraint that sums across rows (only masked rows if WHEN)
            for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                idx_t decide_var_idx = eval_const.variable_indices[term_idx];

                if (decide_var_idx != DConstants::INVALID_INDEX) {
                    // Add entry for each row's variable with its specific coefficient
                    for (idx_t row = 0; row < num_rows; row++) {
                        // PackDB WHEN: skip rows where condition is false
                        if (has_mask && !eval_const.row_mask[row]) {
                            continue;
                        }
                        double coeff = eval_const.row_coefficients[term_idx][row];

                        idx_t var_idx = row * num_decide_vars + decide_var_idx;
                        a_rows.push_back(constraint_idx);
                        a_cols.push_back(var_idx);
                        a_vals.push_back(coeff);
                    }
                }
            }

            // Set constraint bounds
            double rhs = eval_const.rhs_values[0]; // Same for all rows

            if (eval_const.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
                row_lower.push_back(rhs);
                row_upper.push_back(1e30);
            } else if (eval_const.comparison_type == ExpressionType::COMPARE_GREATERTHAN) {
                // Integer model: sum(x) > c  => sum(x) >= floor(c) + 1
                double lb = std::floor(rhs) + 1.0;
                row_lower.push_back(lb);
                row_upper.push_back(1e30);
            } else if (eval_const.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
                row_lower.push_back(-1e30);
                row_upper.push_back(rhs);
            } else if (eval_const.comparison_type == ExpressionType::COMPARE_LESSTHAN) {
                // Integer model: sum(x) < c  => sum(x) <= ceil(c) - 1
                double ub = std::ceil(rhs) - 1.0;
                row_lower.push_back(-1e30);
                row_upper.push_back(ub);
            } else if (eval_const.comparison_type == ExpressionType::COMPARE_EQUAL) {
                row_lower.push_back(rhs);
                row_upper.push_back(rhs);
            }

            constraint_idx++;

        } else {
            // PER-ROW CONSTRAINT: Create separate constraint for each row [WHEN condition]

            for (idx_t row = 0; row < num_rows; row++) {
                // PackDB WHEN: skip rows where condition is false
                if (has_mask && !eval_const.row_mask[row]) {
                    continue;
                }

                // Build constraint for this row
                for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                    idx_t decide_var_idx = eval_const.variable_indices[term_idx];

                    if (decide_var_idx != DConstants::INVALID_INDEX) {
                        double coeff = eval_const.row_coefficients[term_idx][row];
                        idx_t var_idx = row * num_decide_vars + decide_var_idx;

                        a_rows.push_back(constraint_idx);
                        a_cols.push_back(var_idx);
                        a_vals.push_back(coeff);
                    }
                }

                // Set constraint bounds based on comparison type
                double rhs = eval_const.rhs_values[row];

                if (eval_const.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
                    row_lower.push_back(rhs);
                    row_upper.push_back(1e30);
                } else if (eval_const.comparison_type == ExpressionType::COMPARE_GREATERTHAN) {
                    // Integer model: x > c  => x >= floor(c) + 1
                    double lb = std::floor(rhs) + 1.0;
                    row_lower.push_back(lb);
                    row_upper.push_back(1e30);
                } else if (eval_const.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
                    row_lower.push_back(-1e30);
                    row_upper.push_back(rhs);
                } else if (eval_const.comparison_type == ExpressionType::COMPARE_LESSTHAN) {
                    // Integer model: x < c  => x <= ceil(c) - 1
                    double ub = std::ceil(rhs) - 1.0;
                    row_lower.push_back(-1e30);
                    row_upper.push_back(ub);
                } else if (eval_const.comparison_type == ExpressionType::COMPARE_EQUAL) {
                    row_lower.push_back(rhs);
                    row_upper.push_back(rhs);
                }

                constraint_idx++;
            }
        }
    }

    //===--------------------------------------------------------------------===//
    // 3. Build HighsLp and convert COO to CSR
    //===--------------------------------------------------------------------===//

    HighsLp lp;
    lp.num_col_ = total_vars;
    lp.num_row_ = num_constraints;
    lp.sense_ = sense;
    lp.offset_ = 0.0;
    lp.col_cost_ = model.obj_coeffs;
    lp.col_lower_ = model.col_lower;
    lp.col_upper_ = model.col_upper;
    lp.row_lower_ = row_lower;
    lp.row_upper_ = row_upper;

    // Convert COO to CSR format
    lp.a_matrix_.format_ = MatrixFormat::kRowwise;
    vector<HighsInt> row_starts(num_constraints + 1, 0);

    for (idx_t i = 0; i < a_rows.size(); i++) {
        row_starts[a_rows[i] + 1]++;
    }
    for (idx_t i = 0; i < num_constraints; i++) {
        row_starts[i + 1] += row_starts[i];
    }

    vector<HighsInt> col_indices(a_vals.size());
    vector<double> values(a_vals.size());
    vector<HighsInt> current_pos = row_starts;

    for (idx_t i = 0; i < a_rows.size(); i++) {
        idx_t row = a_rows[i];
        idx_t pos = current_pos[row];
        col_indices[pos] = a_cols[i];
        values[pos] = a_vals[i];
        current_pos[row]++;
    }

    lp.a_matrix_.start_ = row_starts;
    lp.a_matrix_.index_ = col_indices;
    lp.a_matrix_.value_ = values;

    // Set integrality
    lp.integrality_.resize(total_vars);
    for (idx_t i = 0; i < total_vars; i++) {
        lp.integrality_[i] = var_types[i];
    }

    HighsStatus status = highs.passModel(lp);
    if (status != HighsStatus::kOk) {
        throw InternalException("Failed to pass model to HiGHS: status %d", (int)status);
    }

    //===--------------------------------------------------------------------===//
    // 4. Solve
    //===--------------------------------------------------------------------===//

    status = highs.run();
    if (status != HighsStatus::kOk) {
        HighsModelStatus model_status = highs.getModelStatus();
        throw InternalException("HiGHS solver failed: status %d, model_status %d", (int)status, (int)model_status);
    }

    //===--------------------------------------------------------------------===//
    // 5. Check status
    //===--------------------------------------------------------------------===//

    HighsModelStatus model_status = highs.getModelStatus();
    if (model_status != HighsModelStatus::kOptimal) {
        if (model_status == HighsModelStatus::kInfeasible) {
            throw InvalidInputException(
                "DECIDE optimization is infeasible: No valid solution exists that satisfies all constraints.\n\n"
                "This means the SUCH THAT conditions cannot all be met simultaneously.\n\n"
                "Common causes:\n"
                "  • Contradictory bounds (e.g., x >= 10 AND x <= 5)\n"
                "  • SUM constraints impossible to satisfy with available data\n"
                "  • Variable types too restrictive (BOOLEAN when INTEGER needed)\n\n"
                "Suggestion: Try relaxing constraints or verify input data.");
        } else if (model_status == HighsModelStatus::kUnbounded) {
            throw InvalidInputException(
                "DECIDE optimization is unbounded: The objective can grow infinitely.\n\n"
                "This means the MAXIMIZE/MINIMIZE goal has no finite optimal value.\n"
                "You must add constraints to bound the decision variables.\n\n"
                "Examples:\n"
                "  • Add upper bounds: SUCH THAT x <= 100\n"
                "  • Add budget limits: SUCH THAT SUM(x * cost) <= budget\n"
                "  • Use BOOLEAN instead of INTEGER for selection problems");
        } else if (model_status == HighsModelStatus::kTimeLimit) {
            throw InvalidInputException(
                "DECIDE optimization exceeded time limit.\n"
                "The problem may be too complex to solve in reasonable time.\n"
                "Try simplifying constraints or reducing data size.");
        } else if (model_status == HighsModelStatus::kIterationLimit) {
            throw InvalidInputException(
                "DECIDE optimization exceeded iteration limit.\n"
                "The problem may be too complex. Try simplifying constraints.");
        } else {
            throw InvalidInputException(
                "DECIDE optimization failed with solver status %d.\n"
                "The optimization could not find a solution.\n"
                "This may indicate a problem with the constraints or objective.",
                (int)model_status);
        }
    }

    //===--------------------------------------------------------------------===//
    // 6. Extract solution
    //===--------------------------------------------------------------------===//

    const HighsSolution& solution = highs.getSolution();

    if (solution.col_value.size() < total_vars) {
        throw InternalException(
            "HiGHS returned incomplete solution: expected %llu variables, got %llu",
            total_vars, (idx_t)solution.col_value.size());
    }

    vector<double> result(total_vars);
    for (idx_t i = 0; i < total_vars; i++) {
        double val = solution.col_value[i];
        if (!std::isfinite(val)) {
            throw InternalException(
                "HiGHS returned invalid solution value (NaN or Infinity) for variable %llu", i);
        }
        result[i] = val;
    }

    return result;
}

} // namespace duckdb
