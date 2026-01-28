#include "duckdb/packdb/naive/deterministic_naive.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/packdb/utility/debug.hpp"
#include "Highs.h"

#include <cmath>

namespace duckdb {

vector<double> DeterministicNaive::Solve(const SolverInput& input) {
    idx_t num_rows = input.num_rows;
    idx_t num_decide_vars = input.num_decide_vars;
    idx_t total_vars = num_rows * num_decide_vars;

    // Create HiGHS model
    Highs highs;
    // Disable HiGHS console output for production
    highs.setOptionValue("log_to_console", false);

    // Variable indexing: var_index = row_idx * num_decide_vars + decide_var_idx
    // So for row r and DECIDE variable v: index = r * num_decide_vars + v

    //===--------------------------------------------------------------------===//
    // 1. Set up variables with bounds and types
    //===--------------------------------------------------------------------===//

    vector<double> col_lower(total_vars);
    vector<double> col_upper(total_vars);
    vector<HighsVarType> var_types(total_vars);

    // First, determine per-variable types and default bounds
    vector<double> per_var_lower(num_decide_vars);
    vector<double> per_var_upper(num_decide_vars);
    vector<HighsVarType> per_var_types(num_decide_vars);

    for (idx_t var = 0; var < num_decide_vars; var++) {
        auto logical_type = input.variable_types[var];

        // DECIDE variables represent cardinality (count of tuples), so they MUST be integer types
        // REAL/DOUBLE types are not allowed - this would have been caught in the binder
        if (logical_type == LogicalType::DOUBLE || logical_type == LogicalType::FLOAT) {
            throw InternalException(
                "DECIDE variable has DOUBLE type, but DECIDE variables must be INTEGER "
                "(they represent tuple cardinality). This should have been caught in the binder.");
        } else if (logical_type == LogicalType::INTEGER || logical_type == LogicalType::BIGINT) {
            // INTEGER variables: non-negative by default (cardinality >= 0)
            per_var_types[var] = HighsVarType::kInteger;
            per_var_lower[var] = 0.0;
            per_var_upper[var] = 1e30;
        } else if (logical_type == LogicalType::BOOLEAN) {
            // BINARY variables: [0, 1] (either include or don't include)
            per_var_types[var] = HighsVarType::kInteger;
            per_var_lower[var] = 0.0;
            per_var_upper[var] = 1.0;
        } else {
            // Default to INTEGER if type is not explicitly set
            per_var_types[var] = HighsVarType::kInteger;
            per_var_lower[var] = 0.0;
            per_var_upper[var] = 1e30;
        }
    }

    // Override with explicit bounds from constraints
    // In the refactored version, bounds are passed in input
    // We assume input.lower_bounds and input.upper_bounds are initialized with defaults or tighter bounds
    // Wait, the caller (PhysicalDecide) extracts bounds.
    // So we should use the passed bounds if they are tighter than defaults?
    // Or does the caller pass the FINAL bounds?
    // The caller extracts bounds from constraints.
    // Let's assume the caller passes the bounds extracted from constraints.
    // We should merge them with the type-based defaults.
    
    for (idx_t var = 0; var < num_decide_vars; var++) {
        // Apply passed bounds if valid
        // Note: input.lower_bounds/upper_bounds might be uninitialized or default?
        // The caller should initialize them.
        // Let's assume input bounds are the ones extracted from constraints.
        // We need to intersect them with type bounds.
        
        if (var < input.lower_bounds.size()) {
             per_var_lower[var] = std::max(per_var_lower[var], input.lower_bounds[var]);
        }
        if (var < input.upper_bounds.size()) {
             per_var_upper[var] = std::min(per_var_upper[var], input.upper_bounds[var]);
        }
    }

    // Apply per-variable bounds and types to all rows
    for (idx_t row = 0; row < num_rows; row++) {
        for (idx_t var = 0; var < num_decide_vars; var++) {
            idx_t var_idx = row * num_decide_vars + var;
            col_lower[var_idx] = per_var_lower[var];
            col_upper[var_idx] = per_var_upper[var];
            var_types[var_idx] = per_var_types[var];
        }
    }

    //===--------------------------------------------------------------------===//
    // 2. Set up objective function
    //===--------------------------------------------------------------------===//

    vector<double> obj_coeffs(total_vars, 0.0);

    // If objective exists (we check if coefficients are provided)
    if (!input.objective_variable_indices.empty()) {
        for (idx_t term_idx = 0; term_idx < input.objective_variable_indices.size(); term_idx++) {
            idx_t decide_var_idx = input.objective_variable_indices[term_idx];

            for (idx_t row = 0; row < num_rows; row++) {
                idx_t var_idx = row * num_decide_vars + decide_var_idx;
                // Check bounds
                if (term_idx < input.objective_coefficients.size() && row < input.objective_coefficients[term_idx].size()) {
                    obj_coeffs[var_idx] = input.objective_coefficients[term_idx][row];
                }
            }
        }
    }

    // Set sense (maximize or minimize)
    ObjSense sense = (input.sense == DecideSense::MAXIMIZE)
        ? ObjSense::kMaximize
        : ObjSense::kMinimize;

    //===--------------------------------------------------------------------===//
    // 3. Set up constraints
    //===--------------------------------------------------------------------===//

    // Constraint matrix in COO format (row, col, value)
    vector<int> a_rows;
    vector<int> a_cols;
    vector<double> a_vals;
    vector<double> row_lower;
    vector<double> row_upper;

    idx_t constraint_idx = 0;
    for (auto &eval_const : input.constraints) {
        // Use original provenance: aggregate if and only if LHS was an aggregate (e.g., SUM(...))
        bool is_aggregate = eval_const.lhs_is_aggregate;

        if (is_aggregate) {
            // AGGREGATE CONSTRAINT: SUM(x) <= 10
            // Create ONE constraint that sums across ALL rows
            for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                idx_t decide_var_idx = eval_const.variable_indices[term_idx];

                if (decide_var_idx != DConstants::INVALID_INDEX) {
                    // Add entry for each row's variable with its specific coefficient
                    for (idx_t row = 0; row < num_rows; row++) {
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
            // PER-ROW CONSTRAINT: Create separate constraint for each row

            for (idx_t row = 0; row < num_rows; row++) {
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
    // 4. Build HighsLp model and pass to HiGHS
    //===--------------------------------------------------------------------===//

    idx_t num_constraints = constraint_idx; // Total number of constraints created

    // Sanity checks before passing to solver
    if (row_lower.size() != num_constraints || row_upper.size() != num_constraints) {
        throw InternalException("Row bounds size mismatch: row_lower=%llu row_upper=%llu num_constraints=%llu",
            (idx_t)row_lower.size(), (idx_t)row_upper.size(), num_constraints);
    }
    for (idx_t i = 0; i < num_constraints; i++) {
        if (!(std::isfinite(row_lower[i]) || std::isinf(row_lower[i]))) {
            throw InternalException("Row lower bound NaN at row %llu", i);
        }
        if (!(std::isfinite(row_upper[i]) || std::isinf(row_upper[i]))) {
            throw InternalException("Row upper bound NaN at row %llu", i);
        }
        if (row_lower[i] > row_upper[i]) {
            throw InternalException("Infeasible row bounds at row %llu: [%f, %f]", i, row_lower[i], row_upper[i]);
        }
    }
    for (idx_t i = 0; i < a_rows.size(); i++) {
        if ((idx_t)a_rows[i] >= num_constraints) {
            throw InternalException("Constraint matrix row index out of range at nz %llu: row=%d >= %llu",
                i, a_rows[i], num_constraints);
        }
        if ((idx_t)a_cols[i] >= total_vars) {
            throw InternalException("Constraint matrix col index out of range at nz %llu: col=%d >= %llu",
                i, a_cols[i], total_vars);
        }
        if (!std::isfinite(a_vals[i])) {
            throw InternalException("Constraint matrix value not finite at nz %llu: %f", i, a_vals[i]);
        }
    }
    for (idx_t i = 0; i < total_vars; i++) {
        if (!std::isfinite(col_lower[i]) || !std::isfinite(col_upper[i]) || col_lower[i] > col_upper[i]) {
            throw InternalException("Column bounds invalid at col %llu: [%f, %f]", i, col_lower[i], col_upper[i]);
        }
        if (!std::isfinite(obj_coeffs[i])) {
            throw InternalException("Objective coefficient not finite at col %llu: %f", i, obj_coeffs[i]);
        }
    }

    HighsLp lp;
    lp.num_col_ = total_vars;
    lp.num_row_ = num_constraints;
    lp.sense_ = sense;
    lp.offset_ = 0.0;
    lp.col_cost_ = obj_coeffs;
    lp.col_lower_ = col_lower;
    lp.col_upper_ = col_upper;
    lp.row_lower_ = row_lower;
    lp.row_upper_ = row_upper;

    // Constraint matrix in CSR format
    // Convert from COO (row, col, val) to CSR (row pointers, column indices, values)
    lp.a_matrix_.format_ = MatrixFormat::kRowwise;

    // Build CSR format
    vector<HighsInt> row_starts(num_constraints + 1, 0);

    // Count non-zeros per row
    for (idx_t i = 0; i < a_rows.size(); i++) {
        row_starts[a_rows[i] + 1]++;
    }

    // Convert counts to cumulative sum (row start indices)
    for (idx_t i = 0; i < num_constraints; i++) {
        row_starts[i + 1] += row_starts[i];
    }

    // Fill column indices and values
    vector<HighsInt> col_indices(a_vals.size());
    vector<double> values(a_vals.size());
    vector<HighsInt> current_pos = row_starts; // Track current position for each row

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
        lp.integrality_[i] = (var_types[i] == HighsVarType::kInteger) ? HighsVarType::kInteger : HighsVarType::kContinuous;
    }

    HighsStatus status = highs.passModel(lp);

    if (status != HighsStatus::kOk) {
        throw InternalException("Failed to pass model to HiGHS: status %d", (int)status);
    }

    //===--------------------------------------------------------------------===//
    // 5. Solve the ILP
    //===--------------------------------------------------------------------===//

    // Try writing model to a file for debugging if supported
    // highs.writeModel("highs_model.mps");

    status = highs.run();

    if (status != HighsStatus::kOk) {
        // Provide additional context if available
        HighsModelStatus model_status = highs.getModelStatus();
        throw InternalException("HiGHS solver failed: status %d, model_status %d", (int)status, (int)model_status);
    }

    // Get solution info
    HighsModelStatus model_status = highs.getModelStatus();
    // Throw on non-optimal models with descriptive messages
    if (model_status != HighsModelStatus::kOptimal) {
        if (model_status == HighsModelStatus::kInfeasible) {
            throw InvalidInputException(
                "DECIDE optimization is infeasible: No valid solution exists that satisfies all constraints.\n\n"
                "This means the SUCH THAT conditions cannot all be met simultaneously.\n\n"
                "Common causes:\n"
                "  • Contradictory bounds (e.g., x >= 10 AND x <= 5)\n"
                "  • SUM constraints impossible to satisfy with available data\n"
                "  • Variable types too restrictive (BINARY when INTEGER needed)\n\n"
                "Suggestion: Try relaxing constraints or verify input data.");
        } else if (model_status == HighsModelStatus::kUnbounded) {
            throw InvalidInputException(
                "DECIDE optimization is unbounded: The objective can grow infinitely.\n\n"
                "This means the MAXIMIZE/MINIMIZE goal has no finite optimal value.\n"
                "You must add constraints to bound the decision variables.\n\n"
                "Examples:\n"
                "  • Add upper bounds: SUCH THAT x <= 100\n"
                "  • Add budget limits: SUCH THAT SUM(x * cost) <= budget\n"
                "  • Use BINARY instead of INTEGER for selection problems");
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

    // Validate solution completeness
    if (solution.col_value.size() < total_vars) {
        throw InternalException(
            "HiGHS returned incomplete solution: expected %llu variables, got %llu",
            total_vars, (idx_t)solution.col_value.size());
    }

    vector<double> result(total_vars);

    for (idx_t i = 0; i < total_vars; i++) {
        double val = solution.col_value[i];

        // Validate solution values are finite
        if (!std::isfinite(val)) {
            throw InternalException(
                "HiGHS returned invalid solution value (NaN or Infinity) for variable %llu",
                i);
        }

        result[i] = val;
    }

    return result;
}

} // namespace duckdb