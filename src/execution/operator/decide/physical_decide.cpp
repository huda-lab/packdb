#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include "duckdb/packdb/utility/debug.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include "Highs.h"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Expression Analysis Helper Functions
//===--------------------------------------------------------------------===//

idx_t PhysicalDecide::FindDecideVariable(const Expression &expr) const {
    // Base case: check if this is a column reference to a DECIDE variable
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        for (idx_t i = 0; i < decide_variables.size(); i++) {
            auto &decide_var = decide_variables[i]->Cast<BoundColumnRefExpression>();
            if (colref.binding == decide_var.binding) {
                return i;
            }
        }
    }

    // Recursive case: search in children
    idx_t result = DConstants::INVALID_INDEX;
    ExpressionIterator::EnumerateChildren(const_cast<Expression&>(expr),
        [&](unique_ptr<Expression> &child) {
            if (result == DConstants::INVALID_INDEX && child) {
                result = FindDecideVariable(*child);
            }
        });
    return result;
}

bool PhysicalDecide::ContainsVariable(const Expression &expr, idx_t var_idx) const {
    // Check if this expression is the variable we're looking for
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        auto &decide_var = decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
        return colref.binding == decide_var.binding;
    }

    // Recursively check children
    bool found = false;
    ExpressionIterator::EnumerateChildren(const_cast<Expression&>(expr),
        [&](unique_ptr<Expression> &child) {
            if (!found && child && ContainsVariable(*child, var_idx)) {
                found = true;
            }
        });
    return found;
}

unique_ptr<Expression> PhysicalDecide::ExtractCoefficientWithoutVariable(const Expression &expr, idx_t var_idx) const {
    // If this IS the variable itself, return constant 1
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        auto &decide_var = decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
        if (colref.binding == decide_var.binding) {
            return make_uniq<BoundConstantExpression>(Value::INTEGER(1));
        }
    }

    // If it's a multiplication, filter out children containing the variable
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        if (func.function.name == "*") {
            vector<unique_ptr<Expression>> filtered_children;
            for (auto &child : func.children) {
                if (!ContainsVariable(*child, var_idx)) {
                    filtered_children.push_back(child->Copy());
                }
            }

            if (filtered_children.empty()) {
                return make_uniq<BoundConstantExpression>(Value::INTEGER(1));
            }
            if (filtered_children.size() == 1) {
                return std::move(filtered_children[0]);
            }

            // Rebuild multiplication with remaining children
            return make_uniq<BoundFunctionExpression>(func.return_type, func.function,
                                                     std::move(filtered_children), nullptr);
        }
    }

    // If it's a cast, recurse into child
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        return ExtractCoefficientWithoutVariable(*cast.child, var_idx);
    }

    // Otherwise, return a copy of the entire expression (no variable in it)
    return expr.Copy();
}

void PhysicalDecide::ExtractLinearTerms(const Expression &expr, vector<LinearTerm> &out_terms) const {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();

        // Addition: recursively process all children
        if (func.function.name == "+") {
            for (auto &child : func.children) {
                ExtractLinearTerms(*child, out_terms);
            }
            return;
        }

        // Multiplication: extract variable and coefficient
        if (func.function.name == "*") {
            idx_t var_idx = FindDecideVariable(func);

            if (var_idx == DConstants::INVALID_INDEX) {
                // No variable found - this is a constant term
                out_terms.push_back(LinearTerm{DConstants::INVALID_INDEX, func.Copy()});
            } else {
                // Variable found - extract coefficient
                auto coef = ExtractCoefficientWithoutVariable(func, var_idx);
                out_terms.push_back(LinearTerm{var_idx, std::move(coef)});
            }
            return;
        }
    }

    // Handle casts
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        auto &cast = expr.Cast<BoundCastExpression>();
        ExtractLinearTerms(*cast.child, out_terms);
        return;
    }

    // Base case: constant or simple column reference
    idx_t var_idx = FindDecideVariable(expr);
    if (var_idx == DConstants::INVALID_INDEX) {
        // Constant term
        out_terms.push_back(LinearTerm{DConstants::INVALID_INDEX, expr.Copy()});
    } else {
        // Just a variable (coefficient = 1)
        out_terms.push_back(LinearTerm{var_idx,
            make_uniq<BoundConstantExpression>(Value::INTEGER(1))});
    }
}

//===--------------------------------------------------------------------===//
// Constructor
//===--------------------------------------------------------------------===//

PhysicalDecide::PhysicalDecide(vector<LogicalType> types, idx_t estimated_cardinality, 
                    unique_ptr<PhysicalOperator> child, idx_t decide_index, 
                    vector<unique_ptr<Expression>> decide_variables,
                    unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                    unique_ptr<Expression> decide_objective)
    : PhysicalOperator(PhysicalOperatorType::DECIDE, std::move(types), estimated_cardinality)
    , decide_index(decide_index)
    , decide_variables(std::move(decide_variables))
    , decide_constraints(std::move(decide_constraints))
    , decide_sense(decide_sense)
    , decide_objective(std::move(decide_objective)) {
    children.push_back(std::move(child));
}

// OLD STRUCTS - REMOVED (now using LinearConstraint and LinearObjective from header)

//===--------------------------------------------------------------------===//
// Sink (Collecting Data)
//===--------------------------------------------------------------------===//
class DecideGlobalSinkState : public GlobalSinkState {
public:
    explicit DecideGlobalSinkState(ClientContext &context, const PhysicalDecide &op)
        : data(context, op.children[0]->GetTypes()), op(op) {
        // Analyze constraints and objective using new visitor-based approach
        AnalyzeConstraint(op.decide_constraints);
        AnalyzeObjective(op.decide_objective);

        // Minimal: keep constructor lean; detailed solver output comes from HiGHS
    }

    void AnalyzeConstraint(const unique_ptr<Expression>& expr_ptr) {
        auto &expr = *expr_ptr;
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                // Recursively analyze each conjunction child (AND expressions)
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                for (auto &child : conj.children) {
                    AnalyzeConstraint(child);
                }
                break;
            }

            case ExpressionClass::BOUND_COMPARISON: {
                auto &comp = expr.Cast<BoundComparisonExpression>();

                auto constraint = make_uniq<LinearConstraint>();
                constraint->comparison_type = comp.type;
                constraint->rhs_expr = comp.right->Copy();

                // Extract terms from LHS
                Expression *lhs = comp.left.get();
                while (lhs->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    lhs = lhs->Cast<BoundCastExpression>().child.get();
                }

                if (lhs->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
                    // SUM(...) constraint
                    auto &agg = lhs->Cast<BoundAggregateExpression>();
                    op.ExtractLinearTerms(*agg.children[0], constraint->lhs_terms);
                    constraint->lhs_is_aggregate = true;
                } else {
                    // Simple variable constraint (e.g., x <= 5)
                    // Note: We use the original comp.left here to find variables, 
                    // but we should probably use the unwrapped lhs or handle casts in FindDecideVariable (which we do)
                    idx_t var_idx = op.FindDecideVariable(*comp.left);
                    if (var_idx != DConstants::INVALID_INDEX) {
                        constraint->lhs_terms.push_back(LinearTerm{
                            var_idx,
                            make_uniq<BoundConstantExpression>(Value::INTEGER(1))
                        });
                    }
                    constraint->lhs_is_aggregate = false;
                }

                constraints.push_back(std::move(constraint));
                break;
            }

            default:
                break;
        }
    }

    void AnalyzeObjective(const unique_ptr<Expression>& expr_ptr) {
        if (expr_ptr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
            auto &agg = expr_ptr->Cast<BoundAggregateExpression>();

            objective = make_uniq<LinearObjective>();
            op.ExtractLinearTerms(*agg.children[0], objective->terms);
        }
    }

    //===--------------------------------------------------------------------===//
    // Variable Bounds Extraction (Part 3)
    //===--------------------------------------------------------------------===//

    void ExtractVariableBounds(vector<double> &lower_bounds, vector<double> &upper_bounds) {
        // Traverse decide_constraints to find variable-level bounds
        TraverseBoundsConstraints(*op.decide_constraints, lower_bounds, upper_bounds);
    }

    void TraverseBoundsConstraints(const Expression &expr,
                                   vector<double> &lower_bounds,
                                   vector<double> &upper_bounds) {
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                // AND expression - recurse on all children
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                for (auto &child : conj.children) {
                    TraverseBoundsConstraints(*child, lower_bounds, upper_bounds);
                }
                break;
            }

            case ExpressionClass::BOUND_COMPARISON: {
                auto &comp = expr.Cast<BoundComparisonExpression>();

                // Check if this is a variable-level constraint (not SUM)
                if (comp.left->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
                    idx_t var_idx = op.FindDecideVariable(*comp.left);

                    if (var_idx != DConstants::INVALID_INDEX) {
                        // Extract bound value from RHS
                        if (comp.right->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                            auto &rhs = comp.right->Cast<BoundConstantExpression>();

                            // Cast to double - handle both INTEGER and DOUBLE types
                            double bound_value;
                            if (rhs.value.type().id() == LogicalTypeId::INTEGER ||
                                rhs.value.type().id() == LogicalTypeId::BIGINT) {
                                bound_value = static_cast<double>(rhs.value.GetValue<int64_t>());
                            } else if (rhs.value.type().id() == LogicalTypeId::DOUBLE ||
                                       rhs.value.type().id() == LogicalTypeId::FLOAT) {
                                bound_value = rhs.value.GetValue<double>();
                            } else {
                                // Try default cast
                                bound_value = rhs.value.GetValue<double>();
                            }

                            // Apply bound based on comparison type
                            if (comp.type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
                                // x <= bound
                                upper_bounds[var_idx] = std::min(upper_bounds[var_idx], bound_value);
                            } else if (comp.type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
                                // x >= bound
                                lower_bounds[var_idx] = std::max(lower_bounds[var_idx], bound_value);
                            } else if (comp.type == ExpressionType::COMPARE_EQUAL) {
                                // x = bound (if enabled in future)
                                lower_bounds[var_idx] = bound_value;
                                upper_bounds[var_idx] = bound_value;
                            }
                        }
                    }
                }
                break;
            }

            case ExpressionClass::BOUND_CONSTANT: {
                // Type declarations return dummy constants - skip them
                break;
            }

            default:
                break;
        }
    }

    mutex lock;
    // This collection will hold all the data from the child operator
    ColumnDataCollection data;

    const PhysicalDecide &op;

    // NEW: Using LinearConstraint and LinearObjective
    vector<unique_ptr<LinearConstraint>> constraints;
    unique_ptr<LinearObjective> objective;

    //===--------------------------------------------------------------------===//
    // Evaluated Coefficients (Phase 2)
    //===--------------------------------------------------------------------===//

    //! Stores evaluated numeric coefficients for a constraint
    struct EvaluatedConstraint {
        vector<idx_t> variable_indices;           // Which variable for each term
        vector<vector<double>> row_coefficients;  // [term_idx][row_idx] = coefficient value
        vector<double> rhs_values;                // [row_idx] = RHS value
        ExpressionType comparison_type;
        bool lhs_is_aggregate = false;            // True if original LHS was aggregate (e.g., SUM(...))
    };

    vector<EvaluatedConstraint> evaluated_constraints;
    vector<vector<double>> evaluated_objective_coefficients;  // [term_idx][row_idx]
    vector<idx_t> objective_variable_indices;

    // This will hold the solution from the ILP solver
    vector<double> ilp_solution;  // Changed to double for HiGHS compatibility
};

class DecideLocalSinkState : public LocalSinkState {
public:
    explicit DecideLocalSinkState(ClientContext &context, const PhysicalDecide &op)
        : data(context, op.children[0]->GetTypes()) {
        data.InitializeAppend(append_state);
    }

    // A local collection to buffer chunks before merging into the global state
    ColumnDataCollection data;
    ColumnDataAppendState append_state;
};

unique_ptr<GlobalSinkState> PhysicalDecide::GetGlobalSinkState(ClientContext &context) const {
    return make_uniq<DecideGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalDecide::GetLocalSinkState(ExecutionContext &context) const {
    return make_uniq<DecideLocalSinkState>(context.client, *this);
}

SinkResultType PhysicalDecide::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
    auto &lstate = input.local_state.Cast<DecideLocalSinkState>();
    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    ExpressionExecutor coef_executor (context.client);
    ExpressionExecutor rhs_executor (context.client);
    // for (const auto& con : gstate.cons) {
    //     coef_executor.AddExpression(con->coef);
    //     rhs_executor.AddExpression(con->rhs);
    // }
    // coef_executor.AddExpression(gstate.obj->coef);
    lstate.data.Append(lstate.append_state, chunk);
    return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalDecide::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    auto &lstate = input.local_state.Cast<DecideLocalSinkState>();

    lock_guard<mutex> guard(gstate.lock);
    gstate.data.Combine(lstate.data);

    return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalDecide::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    idx_t num_rows = gstate.data.Count();

    // Evaluate coefficients and build the model (solver provides verbose output)

    //===--------------------------------------------------------------------===//
    // PHASE 2: Evaluate Coefficient Expressions
    //===--------------------------------------------------------------------===//

    // 1. Evaluate constraints
    for (idx_t c = 0; c < gstate.constraints.size(); c++) {
        auto &constraint = gstate.constraints[c];

        DecideGlobalSinkState::EvaluatedConstraint eval_const;
        eval_const.comparison_type = constraint->comparison_type;
        // Preserve whether the original LHS was an aggregate (e.g., SUM(...))
        eval_const.lhs_is_aggregate = constraint->lhs_is_aggregate;

        // Initialize result storage
        eval_const.row_coefficients.resize(constraint->lhs_terms.size());

        // Scan data and evaluate LHS coefficients
        ColumnDataScanState scan_state;
        gstate.data.InitializeScan(scan_state);

        DataChunk chunk;
        chunk.Initialize(context, gstate.data.Types());

        // Store variable indices for all terms (before scanning data)
        for (auto &term : constraint->lhs_terms) {
            eval_const.variable_indices.push_back(term.variable_index);
        }

        while (gstate.data.Scan(scan_state, chunk)) {
            // Evaluate each term separately for this chunk
            for (idx_t term_idx = 0; term_idx < constraint->lhs_terms.size(); term_idx++) {
                auto &term = constraint->lhs_terms[term_idx];

                // Transform BoundColumnRefExpression to BoundReferenceExpression
                // The coefficient expressions contain BoundColumnRefExpression which reference
                // columns by table binding, but ExpressionExecutor expects BoundReferenceExpression
                // which reference columns by index in the chunk.
                //
                // The child operator's output is stored in gstate.data. We need to map
                // BoundColumnRefExpression (binding.table_index, binding.column_index) to
                // the corresponding index in gstate.data.
                //
                // Build a mapping: for each column in gstate.data, find its binding
                // Actually, gstate.data.Types() gives us the types, and the columns are in the
                // order they were appended. We stored the child operator's output directly.
                //
                // SIMPLE SOLUTION: The child operator's columns are indexed 0, 1, 2, ...
                // When we stored them in gstate.data, they kept the same order.
                // BoundColumnRefExpression has binding.column_index which should map directly.

                // Transform the coefficient expression to replace BoundColumnRefExpression with BoundReferenceExpression
                std::function<unique_ptr<Expression>(const Expression&)> TransformExpression;
                TransformExpression = [&](const Expression &expr) -> unique_ptr<Expression> {
                    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                        auto &colref = expr.Cast<BoundColumnRefExpression>();
                        // Map to chunk column index - assume column_index maps directly
                        return make_uniq<BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
                    } else if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
                        auto &func = expr.Cast<BoundFunctionExpression>();
                        vector<unique_ptr<Expression>> new_children;
                        for (auto &child : func.children) {
                            new_children.push_back(TransformExpression(*child));
                        }
                        // Copy bind_info if it exists
                        unique_ptr<FunctionData> new_bind_info;
                        if (func.bind_info) {
                            new_bind_info = func.bind_info->Copy();
                        }
                        return make_uniq<BoundFunctionExpression>(func.return_type, func.function, std::move(new_children), std::move(new_bind_info));
                    } else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                        auto &cast = expr.Cast<BoundCastExpression>();
                        // Transform the child and wrap in a new cast
                        auto transformed_child = TransformExpression(*cast.child);
                        return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type, cast.try_cast);
                    } else {
                        // Constants and other expressions: just copy
                        return expr.Copy();
                    }
                };

                auto transformed_coef = TransformExpression(*term.coefficient);

                // Create executor and evaluate this term
                ExpressionExecutor term_executor(context);
                try {
                    term_executor.AddExpression(*transformed_coef);
                } catch (const std::exception &e) {
                    throw InternalException("Failed to add expression for term %llu: %s\nOriginal: %s\nTransformed: %s",
                        term_idx, e.what(), term.coefficient->ToString(), transformed_coef->ToString());
                }

                // Execute on chunk
                // Use the expression's actual return type, then cast to double when extracting
                DataChunk term_result;
                vector<LogicalType> result_types = {transformed_coef->return_type};
                term_result.Initialize(context, result_types);
                term_executor.Execute(chunk, term_result);

                // Extract values and cast to double
                auto &vec = term_result.data[0];
                for (idx_t row_in_chunk = 0; row_in_chunk < chunk.size(); row_in_chunk++) {
                    // Cast to double regardless of the actual type (could be INTEGER, DOUBLE, etc.)
                    Value val = vec.GetValue(row_in_chunk);
                    double double_val = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    eval_const.row_coefficients[term_idx].push_back(double_val);
                }
            }
        }

        // Evaluate RHS
        // After symbolic normalization, RHS should be a scalar constant or aggregate expression
        // The binder ensures RHS has no row-varying terms, only constants and aggregates
        double rhs_constant = 0.0;

        if (constraint->rhs_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
            auto &const_expr = constraint->rhs_expr->Cast<BoundConstantExpression>();
            rhs_constant = const_expr.value.GetValue<double>();
        } else {
            // RHS is a complex expression like "(-15.0 + sum(-11.0))"
            // Use ExpressionExecutor to evaluate it. This handles constants, math functions,
            // and potentially uncorrelated subqueries if supported by the executor.
            try {
                Value val = ExpressionExecutor::EvaluateScalar(context, *constraint->rhs_expr);
                rhs_constant = val.GetValue<double>();
            } catch (const Exception &e) {
                // Fallback or rethrow with context
                throw InternalException("Failed to evaluate DECIDE constraint RHS: %s", e.what());
            }
        }

        // RHS is same for all rows after normalization
        eval_const.rhs_values.resize(num_rows, rhs_constant);

        gstate.evaluated_constraints.push_back(std::move(eval_const));
    }

    // 2. Evaluate objective
    if (gstate.objective) {
        // Transform expressions (same as for constraints)
        std::function<unique_ptr<Expression>(const Expression&)> TransformExpression;
        TransformExpression = [&](const Expression &expr) -> unique_ptr<Expression> {
            if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                auto &colref = expr.Cast<BoundColumnRefExpression>();
                return make_uniq<BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
            } else if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
                auto &func = expr.Cast<BoundFunctionExpression>();
                vector<unique_ptr<Expression>> new_children;
                for (auto &child : func.children) {
                    new_children.push_back(TransformExpression(*child));
                }
                unique_ptr<FunctionData> new_bind_info;
                if (func.bind_info) {
                    new_bind_info = func.bind_info->Copy();
                }
                return make_uniq<BoundFunctionExpression>(func.return_type, func.function, std::move(new_children), std::move(new_bind_info));
            } else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                auto &cast = expr.Cast<BoundCastExpression>();
                auto transformed_child = TransformExpression(*cast.child);
                return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type, cast.try_cast);
            } else {
                return expr.Copy();
            }
        };

        // Build transformed expressions
        vector<unique_ptr<Expression>> transformed_coefficients;
        for (auto &term : gstate.objective->terms) {
            gstate.objective_variable_indices.push_back(term.variable_index);
            transformed_coefficients.push_back(TransformExpression(*term.coefficient));
        }

        gstate.evaluated_objective_coefficients.resize(gstate.objective->terms.size());

        // Scan and evaluate chunk by chunk
        ColumnDataScanState obj_scan_state;
        gstate.data.InitializeScan(obj_scan_state);

        DataChunk obj_chunk;
        obj_chunk.Initialize(context, gstate.data.Types());

        while (gstate.data.Scan(obj_scan_state, obj_chunk)) {
            // Evaluate each term separately
            for (idx_t term_idx = 0; term_idx < transformed_coefficients.size(); term_idx++) {
                ExpressionExecutor term_executor(context);
                term_executor.AddExpression(*transformed_coefficients[term_idx]);

                // Use the expression's actual return type, then cast to double when extracting
                DataChunk term_result;
                vector<LogicalType> result_types = {transformed_coefficients[term_idx]->return_type};
                term_result.Initialize(context, result_types);
                term_executor.Execute(obj_chunk, term_result);

                // Extract values and cast to double
                auto &vec = term_result.data[0];
                for (idx_t row_in_chunk = 0; row_in_chunk < obj_chunk.size(); row_in_chunk++) {
                    // Cast to double regardless of the actual type (could be INTEGER, DOUBLE, etc.)
                    Value val = vec.GetValue(row_in_chunk);
                    double double_val = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
                    gstate.evaluated_objective_coefficients[term_idx].push_back(double_val);
                }
            }
        }

        // No extra debug here; solver output will show timings/objective
    }

    //===--------------------------------------------------------------------===//
    // PHASE 3: Build and Solve ILP with HiGHS
    //===--------------------------------------------------------------------===//

    idx_t num_decide_vars = decide_variables.size();
    idx_t total_vars = num_rows * num_decide_vars;

    // Create HiGHS model
    Highs highs;
    // highs.setOptionValue("output_flag", true); // Show HiGHS output for debugging
    highs.setOptionValue("log_to_console", true);

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
        // Extract type from decide_variables
        auto &decide_var = decide_variables[var]->Cast<BoundColumnRefExpression>();
        auto logical_type = decide_var.return_type;

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

    // Override with explicit bounds from constraints (Part 3)
    gstate.ExtractVariableBounds(per_var_lower, per_var_upper);

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

    if (gstate.objective) {
        for (idx_t term_idx = 0; term_idx < gstate.objective_variable_indices.size(); term_idx++) {
            idx_t decide_var_idx = gstate.objective_variable_indices[term_idx];

            for (idx_t row = 0; row < num_rows; row++) {
                idx_t var_idx = row * num_decide_vars + decide_var_idx;
                obj_coeffs[var_idx] = gstate.evaluated_objective_coefficients[term_idx][row];
            }
        }
    }

    // Set sense (maximize or minimize)
    ObjSense sense = (decide_sense == DecideSense::MAXIMIZE)
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
    for (auto &eval_const : gstate.evaluated_constraints) {
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
            throw InternalException("DECIDE optimization infeasible: constraints cannot be satisfied (model_status=%d)", (int)model_status);
        } else if (model_status == HighsModelStatus::kUnbounded) {
            throw InternalException("DECIDE optimization unbounded: objective can grow without bound (model_status=%d)", (int)model_status);
        } else {
            throw InternalException("DECIDE optimization failed: model_status=%d", (int)model_status);
        }
    }

    //===--------------------------------------------------------------------===//
    // 6. Extract solution
    //===--------------------------------------------------------------------===//

    const HighsSolution& solution = highs.getSolution();
    gstate.ilp_solution.resize(total_vars);

    for (idx_t i = 0; i < total_vars; i++) {
        gstate.ilp_solution[i] = solution.col_value[i];
    }

    // Get objective value
    // Objective details are printed by HiGHS; no additional debug needed

    return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source (Producing Output)
//===--------------------------------------------------------------------===//
class DecideGlobalSourceState : public GlobalSourceState {
public:
    explicit DecideGlobalSourceState(const PhysicalDecide &op, DecideGlobalSinkState &sink) {
        sink.data.InitializeScan(scan_state);
        current_row_offset = 0;
    }

    ColumnDataScanState scan_state;
    idx_t current_row_offset; // Track which row we're at in the solution vector

    idx_t MaxThreads() override {
        return 1; // For simplicity, we'll make the source single-threaded.
    }
};

unique_ptr<GlobalSourceState> PhysicalDecide::GetGlobalSourceState(ClientContext &context) const {
    auto &sink = sink_state->Cast<DecideGlobalSinkState>();
    return make_uniq<DecideGlobalSourceState>(*this, sink);
}

SourceResultType PhysicalDecide::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
    auto &gstate = sink_state->Cast<DecideGlobalSinkState>();
    auto &source_state = input.global_state.Cast<DecideGlobalSourceState>();

    // Scan the original buffered data
    gstate.data.Scan(source_state.scan_state, chunk);
    if (chunk.size() == 0) {
        return SourceResultType::FINISHED;
    }

    idx_t num_decide_vars = decide_variables.size();
    idx_t chunk_size = chunk.size();

    // Fill in the DECIDE variable columns with solution values from ILP solver
    for (idx_t decide_var_idx = 0; decide_var_idx < num_decide_vars; decide_var_idx++) {
        // The DECIDE columns are appended at the end of the output
        idx_t column_idx = types.size() - num_decide_vars + decide_var_idx;

        auto &output_vector = chunk.data[column_idx];

        // Set vector to flat (each row has its own value)
        output_vector.SetVectorType(VectorType::FLAT_VECTOR);

        // Get the logical type for this DECIDE variable
        auto &decide_var = decide_variables[decide_var_idx]->Cast<BoundColumnRefExpression>();
        auto var_type = decide_var.return_type;

        // Get data pointer once based on type
        if (var_type == LogicalType::INTEGER || var_type == LogicalType::BIGINT) {
            // Use int32_t for INTEGER, int64_t for BIGINT
            if (var_type == LogicalType::INTEGER) {
                auto output_data = FlatVector::GetData<int32_t>(output_vector);

                for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                    idx_t global_row = source_state.current_row_offset + row_in_chunk;
                    idx_t solution_idx = global_row * num_decide_vars + decide_var_idx;

                    double solution_value = 0.0;
                    if (solution_idx < gstate.ilp_solution.size()) {
                        solution_value = gstate.ilp_solution[solution_idx];
                    }
                    int32_t int_value = static_cast<int32_t>(std::round(solution_value));
                    output_data[row_in_chunk] = int_value;
                }
            } else { // BIGINT
                auto output_data = FlatVector::GetData<int64_t>(output_vector);

                for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                    idx_t global_row = source_state.current_row_offset + row_in_chunk;
                    idx_t solution_idx = global_row * num_decide_vars + decide_var_idx;

                    double solution_value = 0.0;
                    if (solution_idx < gstate.ilp_solution.size()) {
                        solution_value = gstate.ilp_solution[solution_idx];
                    }
                    int64_t int_value = static_cast<int64_t>(std::round(solution_value));
                    output_data[row_in_chunk] = int_value;
                }
            }

        } else if (var_type == LogicalType::BOOLEAN) {
            auto output_data = FlatVector::GetData<bool>(output_vector);

            for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                idx_t global_row = source_state.current_row_offset + row_in_chunk;
                idx_t solution_idx = global_row * num_decide_vars + decide_var_idx;

                double solution_value = 0.0;
                if (solution_idx < gstate.ilp_solution.size()) {
                    solution_value = gstate.ilp_solution[solution_idx];
                }
                output_data[row_in_chunk] = (solution_value >= 0.5);
            }

        } else if (var_type == LogicalType::DOUBLE) {
            auto output_data = FlatVector::GetData<double>(output_vector);

            for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                idx_t global_row = source_state.current_row_offset + row_in_chunk;
                idx_t solution_idx = global_row * num_decide_vars + decide_var_idx;

                double solution_value = 0.0;
                if (solution_idx < gstate.ilp_solution.size()) {
                    solution_value = gstate.ilp_solution[solution_idx];
                }
                output_data[row_in_chunk] = solution_value;
            }

        } else {
            // Default to INTEGER
            auto output_data = FlatVector::GetData<int64_t>(output_vector);

            for (idx_t row_in_chunk = 0; row_in_chunk < chunk_size; row_in_chunk++) {
                idx_t global_row = source_state.current_row_offset + row_in_chunk;
                idx_t solution_idx = global_row * num_decide_vars + decide_var_idx;

                double solution_value = 0.0;
                if (solution_idx < gstate.ilp_solution.size()) {
                    solution_value = gstate.ilp_solution[solution_idx];
                }
                output_data[row_in_chunk] = static_cast<int64_t>(std::round(solution_value));
            }
        }
    }

    // Update row offset for next chunk
    source_state.current_row_offset += chunk_size;

    return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb