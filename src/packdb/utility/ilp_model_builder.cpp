#include "duckdb/packdb/ilp_model.hpp"
#include "duckdb/common/exception.hpp"

#include <cmath>
#include <map>
#include <unordered_map>

namespace duckdb {

// Shared logic for Build and BuildRef — populates all fields except entity data source
static void BuildVarIndexerCommon(VarIndexer &idx, const SolverInput &input,
                                   const vector<EntityMapping> &entity_mappings) {
    idx.num_rows = input.num_rows;
    idx_t num_decide_vars = input.num_decide_vars;

    idx.is_entity_scoped.resize(num_decide_vars, false);
    idx.row_var_offset.resize(num_decide_vars, DConstants::INVALID_INDEX);
    idx.entity_var_base.resize(num_decide_vars, DConstants::INVALID_INDEX);
    idx.var_entity_mapping_idx.resize(num_decide_vars, DConstants::INVALID_INDEX);

    // Classify variables and compute offsets
    idx_t row_var_count = 0;
    for (idx_t v = 0; v < num_decide_vars; v++) {
        bool scoped = !input.variable_entity_scope.empty() &&
                      v < input.variable_entity_scope.size() &&
                      input.variable_entity_scope[v] != DConstants::INVALID_INDEX;
        idx.is_entity_scoped[v] = scoped;
        if (!scoped) {
            idx.row_var_offset[v] = row_var_count;
            row_var_count++;
        }
    }
    idx.num_row_vars = row_var_count;

    // Row block: num_rows * num_row_vars
    idx.entity_block_start = input.num_rows * row_var_count;

    // Entity block: assign base offsets per entity-scoped variable
    idx_t entity_offset = 0;
    for (idx_t v = 0; v < num_decide_vars; v++) {
        if (!idx.is_entity_scoped[v]) {
            continue;
        }
        idx_t scope_idx = input.variable_entity_scope[v];
        D_ASSERT(scope_idx < entity_mappings.size());
        idx.var_entity_mapping_idx[v] = scope_idx;
        idx.entity_var_base[v] = idx.entity_block_start + entity_offset;
        entity_offset += entity_mappings[scope_idx].num_entities;
    }

    idx.global_block_start = idx.entity_block_start + entity_offset;
    idx.total_vars = idx.global_block_start + input.num_global_vars;
}

VarIndexer VarIndexer::Build(const SolverInput &input) {
    VarIndexer idx;
    // Own a copy so this VarIndexer can outlive the SolverInput
    idx.entity_mappings_owned = input.entity_mappings;
    idx.entity_mappings_ref = nullptr;
    BuildVarIndexerCommon(idx, input, idx.entity_mappings_owned);
    return idx;
}

VarIndexer VarIndexer::BuildRef(const SolverInput &input) {
    VarIndexer idx;
    // Reference without copying — caller must ensure SolverInput outlives this VarIndexer
    idx.entity_mappings_ref = &input.entity_mappings;
    BuildVarIndexerCommon(idx, input, input.entity_mappings);
    return idx;
}

SolverModel SolverModel::Build(const SolverInput &input) {
    SolverModel model;

    idx_t num_rows = input.num_rows;
    idx_t num_decide_vars = input.num_decide_vars;

    // Build VarIndexer for mixed row-scoped / entity-scoped variable support.
    // Use BuildRef — the SolverInput outlives this VarIndexer (both are local to Build).
    VarIndexer indexer = VarIndexer::BuildRef(input);
    idx_t total_vars = indexer.total_vars;

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
            per_var_binary[var] = false;
            per_var_lower[var] = 0.0;
            per_var_upper[var] = 1e30;
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

    // Expand per-variable config to all solver variables
    model.col_lower.resize(total_vars);
    model.col_upper.resize(total_vars);
    model.is_integer.resize(total_vars, false);
    model.is_binary.resize(total_vars, false);

    for (idx_t var = 0; var < num_decide_vars; var++) {
        idx_t num_instances = indexer.NumInstances(var);
        for (idx_t inst = 0; inst < num_instances; inst++) {
            idx_t var_idx;
            if (!indexer.is_entity_scoped[var]) {
                var_idx = inst * indexer.num_row_vars + indexer.row_var_offset[var];
            } else {
                var_idx = indexer.entity_var_base[var] + inst;
            }
            model.col_lower[var_idx] = per_var_lower[var];
            model.col_upper[var_idx] = per_var_upper[var];
            model.is_integer[var_idx] = !(input.variable_types[var] == LogicalType::DOUBLE ||
                                          input.variable_types[var] == LogicalType::FLOAT);
            model.is_binary[var_idx] = per_var_binary[var];
        }
    }

    // Append global auxiliary variables after row+entity blocks
    for (idx_t g = 0; g < input.num_global_vars; g++) {
        idx_t var_idx = indexer.global_block_start + g;
        auto gtype = input.global_variable_types[g];
        model.col_lower[var_idx] = input.global_lower_bounds[g];
        model.col_upper[var_idx] = input.global_upper_bounds[g];
        model.is_integer[var_idx] = !(gtype == LogicalType::DOUBLE || gtype == LogicalType::FLOAT);
        model.is_binary[var_idx] = (gtype == LogicalType::BOOLEAN);
    }

    //===--------------------------------------------------------------------===//
    // 2. Set up objective function
    //===--------------------------------------------------------------------===//

    model.obj_coeffs.resize(total_vars, 0.0);

    // Set objective coefficients for global variables
    for (idx_t g = 0; g < input.num_global_vars; g++) {
        idx_t var_idx = indexer.global_block_start + g;
        if (g < input.global_obj_coeffs.size()) {
            model.obj_coeffs[var_idx] = input.global_obj_coeffs[g];
        }
    }
    model.maximize = (input.sense == DecideSense::MAXIMIZE);

    if (!input.objective_variable_indices.empty()) {
        for (idx_t term_idx = 0; term_idx < input.objective_variable_indices.size(); term_idx++) {
            idx_t decide_var_idx = input.objective_variable_indices[term_idx];

            for (idx_t row = 0; row < num_rows; row++) {
                idx_t var_idx = indexer.Get(decide_var_idx, row);
                if (term_idx < input.objective_coefficients.size() &&
                    row < input.objective_coefficients[term_idx].size()) {
                    // Use += because entity-scoped vars: multiple rows map to same solver var
                    model.obj_coeffs[var_idx] += input.objective_coefficients[term_idx][row];
                }
            }
        }
    }

    //===--------------------------------------------------------------------===//
    // 2b. Build quadratic objective (Q matrix) if present
    //===--------------------------------------------------------------------===//

    if (input.has_quadratic_objective && !input.quadratic_inner_variable_indices.empty()) {
        model.has_quadratic_obj = true;
        double q_sign = input.quadratic_sign;  // +1.0 or -1.0

        // Determine convexity: non-convex when sign and sense conflict
        // MAXIMIZE + PSD (sign>0) or MINIMIZE + NSD (sign<0) → non-convex
        bool is_maximize = (input.sense == DecideSense::MAXIMIZE);
        model.nonconvex_quadratic = (q_sign > 0.0) == is_maximize;

        idx_t num_q_terms = input.quadratic_inner_variable_indices.size();

        // Accumulate Q in a map: (var_i, var_j) -> value (lower triangle only, var_i >= var_j)
        std::map<std::pair<int,int>, double> q_map;

        for (idx_t row = 0; row < num_rows; row++) {
            // Collect per-row coefficients for variable terms
            struct VarCoeff { int flat_idx; double coeff; };
            vector<VarCoeff> row_terms;

            for (idx_t t = 0; t < num_q_terms; t++) {
                if (t >= input.quadratic_inner_coefficients.size() ||
                    row >= input.quadratic_inner_coefficients[t].size()) {
                    continue;
                }
                idx_t decide_var_idx = input.quadratic_inner_variable_indices[t];
                double a = input.quadratic_inner_coefficients[t][row];
                if (a == 0.0) continue;

                if (decide_var_idx == DConstants::INVALID_INDEX) {
                    continue;
                }

                int flat_idx = static_cast<int>(indexer.Get(decide_var_idx, row));
                row_terms.push_back({flat_idx, a});
            }

            for (idx_t i = 0; i < row_terms.size(); i++) {
                for (idx_t j = 0; j <= i; j++) {
                    int ri = row_terms[i].flat_idx;
                    int rj = row_terms[j].flat_idx;
                    int q_row = std::max(ri, rj);
                    int q_col = std::min(ri, rj);
                    double val = q_sign * 2.0 * row_terms[i].coeff * row_terms[j].coeff;
                    q_map[{q_row, q_col}] += val;
                }
            }

            // Handle constant term contributions to linear objective
            double c_row = 0.0;
            for (idx_t t = 0; t < num_q_terms; t++) {
                if (t < input.quadratic_inner_coefficients.size() &&
                    row < input.quadratic_inner_coefficients[t].size() &&
                    input.quadratic_inner_variable_indices[t] == DConstants::INVALID_INDEX) {
                    c_row += input.quadratic_inner_coefficients[t][row];
                }
            }
            if (c_row != 0.0) {
                for (auto &vt : row_terms) {
                    model.obj_coeffs[vt.flat_idx] += q_sign * 2.0 * c_row * vt.coeff;
                }
            }
        }

        // Convert map to COO vectors
        model.q_rows.reserve(q_map.size());
        model.q_cols.reserve(q_map.size());
        model.q_vals.reserve(q_map.size());
        for (auto &entry : q_map) {
            if (entry.second == 0.0) continue;
            model.q_rows.push_back(entry.first.first);
            model.q_cols.push_back(entry.first.second);
            model.q_vals.push_back(entry.second);
        }
    }

    //===--------------------------------------------------------------------===//
    // 3. Build constraints
    //===--------------------------------------------------------------------===//

    // Helper: apply comparison sense to a constraint
    auto ApplyComparisonSense = [](ModelConstraint &constr, ExpressionType cmp, double rhs) {
        if (cmp == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
            constr.sense = '>'; constr.rhs = rhs;
        } else if (cmp == ExpressionType::COMPARE_GREATERTHAN) {
            constr.sense = '>'; constr.rhs = std::floor(rhs) + 1.0;
        } else if (cmp == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
            constr.sense = '<'; constr.rhs = rhs;
        } else if (cmp == ExpressionType::COMPARE_LESSTHAN) {
            constr.sense = '<'; constr.rhs = std::ceil(rhs) - 1.0;
        } else if (cmp == ExpressionType::COMPARE_EQUAL) {
            constr.sense = '='; constr.rhs = rhs;
        } else {
            throw InternalException("Unsupported comparison type in ILP model builder");
        }
    };

    for (auto &eval_const : input.constraints) {
        bool is_aggregate = eval_const.lhs_is_aggregate;
        bool has_groups = !eval_const.row_group_ids.empty();

        if (is_aggregate) {
            if (!has_groups) {
                // FAST PATH: no WHEN, no PER — one constraint summing all rows
                // Use a map to accumulate coefficients for entity-scoped vars
                // (multiple rows may map to the same solver variable)
                ModelConstraint constr;
                std::unordered_map<int, double> coeff_accum;

                for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                    idx_t decide_var_idx = eval_const.variable_indices[term_idx];

                    if (decide_var_idx != DConstants::INVALID_INDEX) {
                        for (idx_t row = 0; row < num_rows; row++) {
                            double coeff = eval_const.row_coefficients[term_idx][row];
                            int var_idx = static_cast<int>(indexer.Get(decide_var_idx, row));
                            coeff_accum[var_idx] += coeff;
                        }
                    }
                }

                // Flush accumulated coefficients to sparse constraint
                for (auto &pair : coeff_accum) {
                    if (pair.second != 0.0) {
                        constr.indices.push_back(pair.first);
                        constr.coefficients.push_back(pair.second);
                    }
                }

                double rhs = eval_const.rhs_values[0];
                for (idx_t r = 1; r < eval_const.rhs_values.size(); r++) {
                    if (eval_const.rhs_values[r] != rhs) {
                        throw InvalidInputException(
                            "Aggregate constraint (SUM/AVG) requires a scalar right-hand side, "
                            "but the RHS evaluates to different values per row (row 0 = %g, row %llu = %g). "
                            "This can happen with correlated subqueries. "
                            "For per-row bounds, use a per-row constraint (e.g., x <= column) instead.",
                            rhs, r, eval_const.rhs_values[r]);
                    }
                }
                if (eval_const.was_avg_rewrite) {
                    rhs *= static_cast<double>(num_rows);
                }
                ApplyComparisonSense(constr, eval_const.comparison_type, rhs);
                model.constraints.push_back(std::move(constr));

            } else {
                // UNIFIED PATH: WHEN and/or PER — build group→rows index, emit one constraint per group
                vector<vector<idx_t>> group_rows(eval_const.num_groups);
                for (idx_t row = 0; row < num_rows; row++) {
                    idx_t gid = eval_const.row_group_ids[row];
                    if (gid == DConstants::INVALID_INDEX) {
                        continue;
                    }
                    group_rows[gid].push_back(row);
                }

                double rhs = eval_const.rhs_values[0];
                for (idx_t r = 1; r < eval_const.rhs_values.size(); r++) {
                    if (eval_const.rhs_values[r] != rhs) {
                        throw InvalidInputException(
                            "Aggregate PER constraint requires a scalar right-hand side, "
                            "but the RHS evaluates to different values per row (row 0 = %g, row %llu = %g). "
                            "This can happen with correlated subqueries.",
                            rhs, r, eval_const.rhs_values[r]);
                    }
                }

                for (idx_t g = 0; g < eval_const.num_groups; g++) {
                    if (group_rows[g].empty()) {
                        continue;
                    }
                    ModelConstraint constr;
                    std::unordered_map<int, double> coeff_accum;

                    for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                        idx_t decide_var_idx = eval_const.variable_indices[term_idx];

                        if (decide_var_idx != DConstants::INVALID_INDEX) {
                            for (idx_t row : group_rows[g]) {
                                double coeff = eval_const.row_coefficients[term_idx][row];
                                int var_idx = static_cast<int>(indexer.Get(decide_var_idx, row));
                                coeff_accum[var_idx] += coeff;
                            }
                        }
                    }

                    for (auto &pair : coeff_accum) {
                        if (pair.second != 0.0) {
                            constr.indices.push_back(pair.first);
                            constr.coefficients.push_back(pair.second);
                        }
                    }

                    double group_rhs = rhs;
                    if (eval_const.was_avg_rewrite) {
                        group_rhs *= static_cast<double>(group_rows[g].size());
                    }
                    ApplyComparisonSense(constr, eval_const.comparison_type, group_rhs);
                    model.constraints.push_back(std::move(constr));
                }
            }

        } else {
            // PER-ROW CONSTRAINT: one constraint per row
            for (idx_t row = 0; row < num_rows; row++) {
                // Skip rows excluded by WHEN (row_group_ids with INVALID_INDEX)
                if (has_groups && eval_const.row_group_ids[row] == DConstants::INVALID_INDEX) {
                    continue;
                }
                ModelConstraint constr;

                for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                    idx_t decide_var_idx = eval_const.variable_indices[term_idx];

                    if (decide_var_idx != DConstants::INVALID_INDEX) {
                        double coeff = eval_const.row_coefficients[term_idx][row];
                        idx_t var_idx = indexer.Get(decide_var_idx, row);
                        constr.indices.push_back((int)var_idx);
                        constr.coefficients.push_back(coeff);
                    }
                }

                double rhs = eval_const.rhs_values[row];
                ApplyComparisonSense(constr, eval_const.comparison_type, rhs);
                model.constraints.push_back(std::move(constr));
            }
        }
    }

    // Append raw global constraints (for MIN/MAX objective linking, etc.)
    for (auto &raw : input.global_constraints) {
        ModelConstraint constr;
        constr.indices = raw.indices;
        constr.coefficients = raw.coefficients;
        constr.sense = raw.sense;
        constr.rhs = raw.rhs;
        model.constraints.push_back(std::move(constr));
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

    for (idx_t k = 0; k < model.q_vals.size(); k++) {
        if (model.q_rows[k] < 0 || (idx_t)model.q_rows[k] >= total_vars ||
            model.q_cols[k] < 0 || (idx_t)model.q_cols[k] >= total_vars) {
            throw InternalException("Q matrix entry %llu: index out of range (row=%d, col=%d, total_vars=%llu)",
                                    k, model.q_rows[k], model.q_cols[k], total_vars);
        }
        if (model.q_rows[k] < model.q_cols[k]) {
            throw InternalException("Q matrix entry %llu: not lower triangle (row=%d < col=%d)",
                                    k, model.q_rows[k], model.q_cols[k]);
        }
        if (!std::isfinite(model.q_vals[k])) {
            throw InternalException("Q matrix entry %llu: value not finite: %f", k, model.q_vals[k]);
        }
    }

    return model;
}

} // namespace duckdb
