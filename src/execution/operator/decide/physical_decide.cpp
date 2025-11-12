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

        // Debug output
        deb("=== Constraint Analysis Complete ===");
        deb("Extracted", constraints.size(), "constraints");
        for (idx_t c = 0; c < constraints.size(); c++) {
            deb("Constraint", c, "has", constraints[c]->lhs_terms.size(), "terms,",
                "comparison:", EnumUtil::ToString(constraints[c]->comparison_type));
            for (idx_t t = 0; t < constraints[c]->lhs_terms.size(); t++) {
                auto &term = constraints[c]->lhs_terms[t];
                if (term.variable_index == DConstants::INVALID_INDEX) {
                    deb("  Term", t, ": CONSTANT, coef =", term.coefficient->ToString());
                } else {
                    deb("  Term", t, ": var", term.variable_index, ", coef =", term.coefficient->ToString());
                }
            }
            deb("  RHS:", constraints[c]->rhs_expr->ToString());
        }

        if (objective) {
            deb("=== Objective Analysis Complete ===");
            deb("Objective has", objective->terms.size(), "terms");
            for (idx_t t = 0; t < objective->terms.size(); t++) {
                auto &term = objective->terms[t];
                if (term.variable_index == DConstants::INVALID_INDEX) {
                    deb("  Term", t, ": CONSTANT, coef =", term.coefficient->ToString());
                } else {
                    deb("  Term", t, ": var", term.variable_index, ", coef =", term.coefficient->ToString());
                }
            }
        }
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
                if (comp.left->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
                    // SUM(...) constraint
                    auto &agg = comp.left->Cast<BoundAggregateExpression>();
                    op.ExtractLinearTerms(*agg.children[0], constraint->lhs_terms);
                } else {
                    // Simple variable constraint (e.g., x <= 5)
                    idx_t var_idx = op.FindDecideVariable(*comp.left);
                    if (var_idx != DConstants::INVALID_INDEX) {
                        constraint->lhs_terms.push_back(LinearTerm{
                            var_idx,
                            make_uniq<BoundConstantExpression>(Value::INTEGER(1))
                        });
                    }
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

    deb("=== Phase 2: Evaluating Coefficient Expressions ===");
    deb("Number of rows:", num_rows);
    deb("Number of constraints:", gstate.constraints.size());

    //===--------------------------------------------------------------------===//
    // PHASE 2: Evaluate Coefficient Expressions
    //===--------------------------------------------------------------------===//

    // 1. Evaluate constraints
    for (idx_t c = 0; c < gstate.constraints.size(); c++) {
        auto &constraint = gstate.constraints[c];

        deb("Evaluating constraint", c, "with", constraint->lhs_terms.size(), "terms");

        DecideGlobalSinkState::EvaluatedConstraint eval_const;
        eval_const.comparison_type = constraint->comparison_type;

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
                DataChunk term_result;
                vector<LogicalType> result_types = {LogicalType::DOUBLE};
                term_result.Initialize(context, result_types);
                term_executor.Execute(chunk, term_result);

                // Extract values
                auto &vec = term_result.data[0];
                for (idx_t row_in_chunk = 0; row_in_chunk < chunk.size(); row_in_chunk++) {
                    double val = vec.GetValue(row_in_chunk).GetValue<double>();
                    eval_const.row_coefficients[term_idx].push_back(val);
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
            // For now, manually evaluate since EvaluateScalar has issues with aggregates
            // After symbolic normalization, RHS should only contain:
            // - Constants
            // - Aggregates of constants (like sum(-11.0) which equals -11.0)
            // We can evaluate this by hand for common cases

            // Simple recursive evaluator for RHS
            std::function<double(const Expression&)> EvaluateRHS;
            EvaluateRHS = [&](const Expression &expr) -> double {
                if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                    return expr.Cast<BoundConstantExpression>().value.GetValue<double>();
                } else if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
                    auto &func = expr.Cast<BoundFunctionExpression>();
                    if (func.function.name == "+") {
                        double sum = 0;
                        for (auto &child : func.children) {
                            sum += EvaluateRHS(*child);
                        }
                        return sum;
                    } else if (func.function.name == "-" || func.function.name == "subtract") {
                        if (func.children.size() == 1) {
                            return -EvaluateRHS(*func.children[0]);
                        } else {
                            return EvaluateRHS(*func.children[0]) - EvaluateRHS(*func.children[1]);
                        }
                    } else if (func.function.name == "*" || func.function.name == "multiply") {
                        double product = 1;
                        for (auto &child : func.children) {
                            product *= EvaluateRHS(*child);
                        }
                        return product;
                    }
                } else if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
                    // Aggregates like sum(-11.0) just return the constant value
                    auto &agg = expr.Cast<BoundAggregateExpression>();
                    if (agg.children.size() > 0) {
                        return EvaluateRHS(*agg.children[0]);
                    }
                    return 0.0;
                }
                throw InternalException("Unsupported RHS expression type: %d", (int)expr.GetExpressionClass());
            };

            rhs_constant = EvaluateRHS(*constraint->rhs_expr);
        }

        // RHS is same for all rows after normalization
        eval_const.rhs_values.resize(num_rows, rhs_constant);

        // Debug: show sample coefficients
        if (num_rows > 0) {
            deb("  Sample coefficients for row 0:");
            for (idx_t t = 0; t < eval_const.variable_indices.size(); t++) {
                if (eval_const.variable_indices[t] != DConstants::INVALID_INDEX) {
                    deb("    Term", t, "(var", eval_const.variable_indices[t], "):",
                        eval_const.row_coefficients[t][0]);
                } else {
                    deb("    Term", t, "(CONSTANT):", eval_const.row_coefficients[t][0]);
                }
            }
            deb("  RHS[0]:", eval_const.rhs_values[0]);
        }

        gstate.evaluated_constraints.push_back(std::move(eval_const));
    }

    // 2. Evaluate objective
    if (gstate.objective) {
        deb("Evaluating objective with", gstate.objective->terms.size(), "terms");

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

                DataChunk term_result;
                vector<LogicalType> result_types = {LogicalType::DOUBLE};
                term_result.Initialize(context, result_types);
                term_executor.Execute(obj_chunk, term_result);

                auto &vec = term_result.data[0];
                for (idx_t row_in_chunk = 0; row_in_chunk < obj_chunk.size(); row_in_chunk++) {
                    double val = vec.GetValue(row_in_chunk).GetValue<double>();
                    gstate.evaluated_objective_coefficients[term_idx].push_back(val);
                }
            }
        }

        // Debug: show sample objective coefficients
        if (num_rows > 0) {
            deb("  Sample objective coefficients for row 0:");
            for (idx_t t = 0; t < gstate.objective_variable_indices.size(); t++) {
                if (gstate.objective_variable_indices[t] != DConstants::INVALID_INDEX) {
                    deb("    Term", t, "(var", gstate.objective_variable_indices[t], "):",
                        gstate.evaluated_objective_coefficients[t][0]);
                }
            }
        }
    }

    deb("=== Phase 2 Complete: All coefficients evaluated ===");

    //===--------------------------------------------------------------------===//
    // PHASE 3: Build and Solve ILP (TODO - Next Phase)
    //===--------------------------------------------------------------------===//

    // For now, still generating dummy solution
    idx_t total_vars = num_rows * decide_variables.size();
    gstate.ilp_solution.resize(total_vars);
    for (idx_t i = 0; i < total_vars; i++) {
        gstate.ilp_solution[i] = static_cast<double>(i % 100);
    }

    return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source (Producing Output)
//===--------------------------------------------------------------------===//
class DecideGlobalSourceState : public GlobalSourceState {
public:
    explicit DecideGlobalSourceState(const PhysicalDecide &op, DecideGlobalSinkState &sink) {
        sink.data.InitializeScan(scan_state);
    }

    ColumnDataScanState scan_state;

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
    
    // types is the output columns
    // children[0]->GetTypes() is the input columns
    // deb(types, children[0]->GetTypes());

    for (idx_t i = 0; i < decide_variables.size(); i++) {
        // The new column is the next available column in the output chunk
        auto &output_vector = chunk.data[types.size() - decide_variables.size() + i];
        // D_ASSERT(output_vector.GetType().id() == LogicalTypeId::DOUBLE);

        // Set all values in this new column to a constant index
        if (i == 0) output_vector.Reference(Value::INTEGER(i+10));
        else output_vector.Reference(Value::DOUBLE(i+10));
        // For now set the values to NULL
        // output_vector.SetVectorType(VectorType::CONSTANT_VECTOR);
        // ConstantVector::SetNull(output_vector, true);
    }
    // The chunk's cardinality was already set by the Scan call.
    return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb