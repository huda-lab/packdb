#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/common/exception.hpp"

#include <cmath>

namespace duckdb {

ILPModel ILPModel::Build(const SolverInput &input) {
    ILPModel model;

    idx_t num_rows = input.num_rows;
    idx_t num_decide_vars = input.num_decide_vars;
    idx_t total_vars = num_rows * num_decide_vars;

    model.num_vars = total_vars;

    //===--------------------------------------------------------------------===//
    // 1. Set up variables with bounds and types
    //===--------------------------------------------------------------------===//

    // Determine per-variable types and default bounds
    vector<double> per_var_lower(num_decide_vars);
    vector<double> per_var_upper(num_decide_vars);
    vector<bool> per_var_binary(num_decide_vars, false);

    for (idx_t var = 0; var < num_decide_vars; var++) {
        auto logical_type = input.variable_types[var];

        if (logical_type == LogicalType::DOUBLE || logical_type == LogicalType::FLOAT) {
            throw InternalException(
                "DECIDE variable has DOUBLE type, but DECIDE variables must be INTEGER "
                "(they represent tuple cardinality). This should have been caught in the binder.");
        } else if (logical_type == LogicalType::BOOLEAN) {
            per_var_binary[var] = true;
            per_var_lower[var] = 0.0;
            per_var_upper[var] = 1.0;
        } else {
            // INTEGER / BIGINT or default
            per_var_lower[var] = 0.0;
            per_var_upper[var] = 1e30;
        }
    }

    // Merge with explicit bounds from input (intersect with type-based defaults)
    for (idx_t var = 0; var < num_decide_vars; var++) {
        if (var < input.lower_bounds.size()) {
            per_var_lower[var] = std::max(per_var_lower[var], input.lower_bounds[var]);
        }
        if (var < input.upper_bounds.size()) {
            per_var_upper[var] = std::min(per_var_upper[var], input.upper_bounds[var]);
        }
    }

    // Expand per-variable config to all rows
    model.col_lower.resize(total_vars);
    model.col_upper.resize(total_vars);
    model.is_integer.resize(total_vars, true);  // All DECIDE vars are integer
    model.is_binary.resize(total_vars, false);

    for (idx_t row = 0; row < num_rows; row++) {
        for (idx_t var = 0; var < num_decide_vars; var++) {
            idx_t var_idx = row * num_decide_vars + var;
            model.col_lower[var_idx] = per_var_lower[var];
            model.col_upper[var_idx] = per_var_upper[var];
            model.is_binary[var_idx] = per_var_binary[var];
        }
    }

    //===--------------------------------------------------------------------===//
    // 2. Set up objective function
    //===--------------------------------------------------------------------===//

    model.obj_coeffs.resize(total_vars, 0.0);
    model.maximize = (input.sense == DecideSense::MAXIMIZE);

    if (!input.objective_variable_indices.empty()) {
        for (idx_t term_idx = 0; term_idx < input.objective_variable_indices.size(); term_idx++) {
            idx_t decide_var_idx = input.objective_variable_indices[term_idx];

            for (idx_t row = 0; row < num_rows; row++) {
                idx_t var_idx = row * num_decide_vars + decide_var_idx;
                if (term_idx < input.objective_coefficients.size() &&
                    row < input.objective_coefficients[term_idx].size()) {
                    model.obj_coeffs[var_idx] = input.objective_coefficients[term_idx][row];
                }
            }
        }
    }

    //===--------------------------------------------------------------------===//
    // 3. Build constraints
    //===--------------------------------------------------------------------===//

    for (auto &eval_const : input.constraints) {
        bool is_aggregate = eval_const.lhs_is_aggregate;
        bool has_mask = !eval_const.row_mask.empty();

        if (is_aggregate) {
            // AGGREGATE CONSTRAINT: one constraint summing across all rows
            ILPConstraint constr;

            for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                idx_t decide_var_idx = eval_const.variable_indices[term_idx];

                if (decide_var_idx != DConstants::INVALID_INDEX) {
                    for (idx_t row = 0; row < num_rows; row++) {
                        if (has_mask && !eval_const.row_mask[row]) {
                            continue;
                        }
                        double coeff = eval_const.row_coefficients[term_idx][row];
                        idx_t var_idx = row * num_decide_vars + decide_var_idx;
                        constr.indices.push_back((int)var_idx);
                        constr.coefficients.push_back(coeff);
                    }
                }
            }

            double rhs = eval_const.rhs_values[0];

            if (eval_const.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
                constr.sense = '>';
                constr.rhs = rhs;
            } else if (eval_const.comparison_type == ExpressionType::COMPARE_GREATERTHAN) {
                constr.sense = '>';
                constr.rhs = std::floor(rhs) + 1.0;
            } else if (eval_const.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
                constr.sense = '<';
                constr.rhs = rhs;
            } else if (eval_const.comparison_type == ExpressionType::COMPARE_LESSTHAN) {
                constr.sense = '<';
                constr.rhs = std::ceil(rhs) - 1.0;
            } else if (eval_const.comparison_type == ExpressionType::COMPARE_EQUAL) {
                constr.sense = '=';
                constr.rhs = rhs;
            } else {
                throw InternalException("Unsupported comparison type in ILP model builder");
            }

            model.constraints.push_back(std::move(constr));

        } else {
            // PER-ROW CONSTRAINT: one constraint per row
            for (idx_t row = 0; row < num_rows; row++) {
                if (has_mask && !eval_const.row_mask[row]) {
                    continue;
                }
                ILPConstraint constr;

                for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                    idx_t decide_var_idx = eval_const.variable_indices[term_idx];

                    if (decide_var_idx != DConstants::INVALID_INDEX) {
                        double coeff = eval_const.row_coefficients[term_idx][row];
                        idx_t var_idx = row * num_decide_vars + decide_var_idx;
                        constr.indices.push_back((int)var_idx);
                        constr.coefficients.push_back(coeff);
                    }
                }

                double rhs = eval_const.rhs_values[row];

                if (eval_const.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
                    constr.sense = '>';
                    constr.rhs = rhs;
                } else if (eval_const.comparison_type == ExpressionType::COMPARE_GREATERTHAN) {
                    constr.sense = '>';
                    constr.rhs = std::floor(rhs) + 1.0;
                } else if (eval_const.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
                    constr.sense = '<';
                    constr.rhs = rhs;
                } else if (eval_const.comparison_type == ExpressionType::COMPARE_LESSTHAN) {
                    constr.sense = '<';
                    constr.rhs = std::ceil(rhs) - 1.0;
                } else if (eval_const.comparison_type == ExpressionType::COMPARE_EQUAL) {
                    constr.sense = '=';
                    constr.rhs = rhs;
                } else {
                    throw InternalException("Unsupported comparison type in ILP model builder");
                }

                model.constraints.push_back(std::move(constr));
            }
        }
    }

    //===--------------------------------------------------------------------===//
    // 4. Sanity checks
    //===--------------------------------------------------------------------===//

    for (idx_t i = 0; i < total_vars; i++) {
        if (!std::isfinite(model.col_lower[i]) || !std::isfinite(model.col_upper[i]) ||
            model.col_lower[i] > model.col_upper[i]) {
            throw InternalException("Column bounds invalid at col %llu: [%f, %f]",
                                    i, model.col_lower[i], model.col_upper[i]);
        }
        if (!std::isfinite(model.obj_coeffs[i])) {
            throw InternalException("Objective coefficient not finite at col %llu: %f",
                                    i, model.obj_coeffs[i]);
        }
    }

    for (idx_t c = 0; c < model.constraints.size(); c++) {
        auto &constr = model.constraints[c];
        if (constr.indices.size() != constr.coefficients.size()) {
            throw InternalException("Constraint %llu: indices/coefficients size mismatch", c);
        }
        for (idx_t j = 0; j < constr.indices.size(); j++) {
            if ((idx_t)constr.indices[j] >= total_vars) {
                throw InternalException("Constraint %llu: variable index %d out of range (>= %llu)",
                                        c, constr.indices[j], total_vars);
            }
            if (!std::isfinite(constr.coefficients[j])) {
                throw InternalException("Constraint %llu: coefficient not finite at position %llu: %f",
                                        c, j, constr.coefficients[j]);
            }
        }
        if (!std::isfinite(constr.rhs) && !std::isinf(constr.rhs)) {
            throw InternalException("Constraint %llu: RHS is NaN", c);
        }
    }

    return model;
}

} // namespace duckdb
