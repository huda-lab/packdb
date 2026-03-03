#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include <cmath>
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include "duckdb/packdb/utility/debug.hpp"
#include "duckdb/packdb/ilp_solver.hpp"
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
            return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
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
                return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
            }
            if (filtered_children.size() == 1) {
                return std::move(filtered_children[0]);
            }

            // Rebuild multiplication with remaining children
            return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function,
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
            make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))});
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

    void AnalyzeConstraint(const unique_ptr<Expression>& expr_ptr,
                           unique_ptr<Expression> when_condition = nullptr,
                           unique_ptr<Expression> per_column = nullptr) {
        auto &expr = *expr_ptr;
        switch (expr.GetExpressionClass()) {
            case ExpressionClass::BOUND_CONJUNCTION: {
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                // PackDB: PER wrapper — outermost layer
                if (conj.alias == PER_CONSTRAINT_TAG && conj.children.size() == 2) {
                    // child[0] = the constraint (possibly WHEN-wrapped)
                    // child[1] = the PER column expression
                    AnalyzeConstraint(conj.children[0], std::move(when_condition),
                                      conj.children[1]->Copy());
                    break;
                }
                // PackDB: Check if this is a WHEN constraint wrapper
                if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
                    // child[0] = the actual constraint, child[1] = the WHEN condition
                    AnalyzeConstraint(conj.children[0], conj.children[1]->Copy(),
                                      std::move(per_column));
                    break;
                }
                // Regular conjunction: recursively analyze each child
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

                // PackDB: Store WHEN condition and PER column if present
                if (when_condition) {
                    constraint->when_condition = std::move(when_condition);
                }
                if (per_column) {
                    constraint->per_column = std::move(per_column);
                }

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
                    idx_t var_idx = op.FindDecideVariable(*comp.left);
                    if (var_idx != DConstants::INVALID_INDEX) {
                        constraint->lhs_terms.push_back(LinearTerm{
                            var_idx,
                            make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))
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
        auto *expr = expr_ptr.get();
        while (expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
            expr = expr->Cast<BoundCastExpression>().child.get();
        }

        // PackDB: Check for WHEN wrapper on objective
        unique_ptr<Expression> when_cond;
        if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
            auto &conj = expr->Cast<BoundConjunctionExpression>();
            if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
                when_cond = conj.children[1]->Copy();
                // Unwrap to get the actual objective expression
                expr = conj.children[0].get();
                while (expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    expr = expr->Cast<BoundCastExpression>().child.get();
                }
            }
        }

        if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
            auto &agg = expr->Cast<BoundAggregateExpression>();

            objective = make_uniq<LinearObjective>();
            op.ExtractLinearTerms(*agg.children[0], objective->terms);
            objective->when_condition = std::move(when_cond);
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
                auto &conj = expr.Cast<BoundConjunctionExpression>();
                // PackDB PER: only recurse into the constraint (child[0]), skip the column
                if (conj.alias == PER_CONSTRAINT_TAG && conj.children.size() == 2) {
                    TraverseBoundsConstraints(*conj.children[0], lower_bounds, upper_bounds);
                    break;
                }
                // PackDB WHEN: only recurse into the constraint (child[0]), skip the condition
                if (conj.alias == WHEN_CONSTRAINT_TAG && conj.children.size() == 2) {
                    TraverseBoundsConstraints(*conj.children[0], lower_bounds, upper_bounds);
                    break;
                }
                // AND expression - recurse on all children
                for (auto &child : conj.children) {
                    TraverseBoundsConstraints(*child, lower_bounds, upper_bounds);
                }
                break;
            }

            case ExpressionClass::BOUND_COMPARISON: {
                auto &comp = expr.Cast<BoundComparisonExpression>();

                // Check if this is a variable-level constraint (not SUM)
                // Handle CASTs wrapping aggregates (e.g., CAST(SUM(x)) >= 10)
                auto *lhs = comp.left.get();
                while (lhs->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    lhs = lhs->Cast<BoundCastExpression>().child.get();
                }

                if (lhs->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
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

    // Uses duckdb::EvaluatedConstraint from deterministic_naive.hpp
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
    return make_uniq_base<GlobalSinkState, DecideGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalDecide::GetLocalSinkState(ExecutionContext &context) const {
    return make_uniq_base<LocalSinkState, DecideLocalSinkState>(context.client, *this);
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

    // Validate input data
    if (num_rows == 0) {
        throw InvalidInputException(
            "DECIDE optimization requires at least one input row. "
            "The query before DECIDE returned no data. "
            "Ensure the FROM/WHERE clauses return rows to optimize over.");
    }

    idx_t num_decide_vars = decide_variables.size();
    if (num_decide_vars == 0) {
        throw InternalException(
            "DECIDE operator has no decision variables "
            "(should have been caught during binding)");
    }

    // Evaluate coefficients and build the model (solver provides verbose output)

    //===--------------------------------------------------------------------===//
    // PHASE 2: Evaluate Coefficient Expressions
    //===--------------------------------------------------------------------===//

    // 1. Evaluate constraints
    for (idx_t c = 0; c < gstate.constraints.size(); c++) {
        auto &constraint = gstate.constraints[c];

        EvaluatedConstraint eval_const;
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
                        return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
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
                        return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function, std::move(new_children), std::move(new_bind_info));
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

                    // Check for NULL values
                    if (val.IsNull()) {
                        throw InvalidInputException(
                            "DECIDE constraint coefficient returned NULL at row %llu. "
                            "NULL values are not allowed in optimization coefficients. "
                            "Use COALESCE() to handle NULLs or filter them with WHERE clause.",
                            eval_const.row_coefficients[term_idx].size());
                    }

                    double double_val = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();

                    // Check for NaN or Infinity
                    if (!std::isfinite(double_val)) {
                        throw InvalidInputException(
                            "DECIDE constraint coefficient contains invalid value (NaN or Infinity) at row %llu. "
                            "Common causes:\n"
                            "  • Division by zero in coefficient expression\n"
                            "  • Arithmetic overflow in calculations\n"
                            "  • NULL values that propagated through math operations\n"
                            "Check your coefficient expressions and input data.",
                            eval_const.row_coefficients[term_idx].size());
                    }

                    eval_const.row_coefficients[term_idx].push_back(double_val);
                }
            }
        }

        // Evaluate RHS
        // RHS can be a constant, an aggregate (scalar), or a row-varying expression (for row-wise constraints)
        
        // Initialize RHS values vector
        eval_const.rhs_values.reserve(num_rows);

        if (constraint->rhs_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
            auto &const_expr = constraint->rhs_expr->Cast<BoundConstantExpression>();
            double rhs_constant = const_expr.value.GetValue<double>();
            eval_const.rhs_values.assign(num_rows, rhs_constant);
        } else {
            // RHS is a complex expression. It might be row-varying (e.g., column ref) or scalar (aggregate).
            // We evaluate it against the data chunks.
            
            // Transform expression to use BoundReferenceExpression (same as LHS)
            std::function<unique_ptr<Expression>(const Expression&)> TransformExpression;
            TransformExpression = [&](const Expression &expr) -> unique_ptr<Expression> {
                if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    auto &colref = expr.Cast<BoundColumnRefExpression>();
                    return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
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
                    return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function, std::move(new_children), std::move(new_bind_info));
                } else if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    auto &cast = expr.Cast<BoundCastExpression>();
                    auto transformed_child = TransformExpression(*cast.child);
                    return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type, cast.try_cast);
                } else if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
                    // Handle count_star() aggregate - replace with num_rows constant
                    auto &agg = expr.Cast<BoundAggregateExpression>();
                    if (agg.function.name == "count_star") {
                        return make_uniq_base<Expression, BoundConstantExpression>(Value::BIGINT(num_rows));
                    }
                    // Other aggregates are not supported in RHS
                    throw InternalException("Unsupported aggregate '%s' in constraint RHS. "
                                          "Only count_star() is supported.", agg.function.name);
                } else {
                    return expr.Copy();
                }
            };

            auto transformed_rhs = TransformExpression(*constraint->rhs_expr);

            // Prepare executor
            ExpressionExecutor rhs_executor(context);
            rhs_executor.AddExpression(*transformed_rhs);

            // Scan data and evaluate
            ColumnDataScanState rhs_scan_state;
            gstate.data.InitializeScan(rhs_scan_state);
            DataChunk rhs_chunk;
            rhs_chunk.Initialize(context, gstate.data.Types());

            while (gstate.data.Scan(rhs_scan_state, rhs_chunk)) {
                DataChunk rhs_result;
                vector<LogicalType> result_types = {transformed_rhs->return_type};
                rhs_result.Initialize(context, result_types);
                rhs_executor.Execute(rhs_chunk, rhs_result);

                auto &vec = rhs_result.data[0];
                for (idx_t row_in_chunk = 0; row_in_chunk < rhs_chunk.size(); row_in_chunk++) {
                    Value val = vec.GetValue(row_in_chunk);

                    // Check for NULL values
                    if (val.IsNull()) {
                        throw InvalidInputException(
                            "DECIDE constraint right-hand side returned NULL at row %llu. "
                            "NULL values are not allowed in optimization constraints. "
                            "Use COALESCE() to handle NULLs or filter them with WHERE clause.",
                            eval_const.rhs_values.size());
                    }

                    double double_val = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();

                    // Check for NaN or Infinity
                    if (!std::isfinite(double_val)) {
                        throw InvalidInputException(
                            "DECIDE constraint right-hand side contains invalid value (NaN or Infinity) at row %llu. "
                            "Common causes:\n"
                            "  • Division by zero in RHS expression\n"
                            "  • Arithmetic overflow in calculations\n"
                            "  • NULL values that propagated through math operations\n"
                            "Check your RHS expressions and input data.",
                            eval_const.rhs_values.size());
                    }

                    eval_const.rhs_values.push_back(double_val);
                }
            }
        }

        // PackDB: Unified WHEN+PER row→group assignment
        // Produces row_group_ids and num_groups for the evaluated constraint.
        // - No WHEN, no PER: row_group_ids stays empty, num_groups = 0 (fast path)
        // - WHEN only: row_group_ids[row] = 0 (matching) or INVALID_INDEX (excluded), num_groups = 1
        // - PER only: row_group_ids[row] = 0..K-1 (group id), INVALID_INDEX for NULL PER values, num_groups = K
        // - WHEN+PER: WHEN filters first, then PER groups the remaining rows
        bool has_when = (constraint->when_condition != nullptr);
        bool has_per = (constraint->per_column != nullptr);

        if (has_when || has_per) {
            // We need a TransformExpr lambda to convert BoundColumnRef → BoundReference
            // for evaluation against data chunks
            std::function<unique_ptr<Expression>(const Expression&)> TransformCondExpr;
            TransformCondExpr = [&](const Expression &cond_expr) -> unique_ptr<Expression> {
                if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    auto &colref = cond_expr.Cast<BoundColumnRefExpression>();
                    return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
                } else if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
                    auto &func = cond_expr.Cast<BoundFunctionExpression>();
                    vector<unique_ptr<Expression>> new_children;
                    for (auto &child : func.children) {
                        new_children.push_back(TransformCondExpr(*child));
                    }
                    unique_ptr<FunctionData> new_bind_info;
                    if (func.bind_info) {
                        new_bind_info = func.bind_info->Copy();
                    }
                    return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function, std::move(new_children), std::move(new_bind_info));
                } else if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    auto &cast = cond_expr.Cast<BoundCastExpression>();
                    auto transformed_child = TransformCondExpr(*cast.child);
                    return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type, cast.try_cast);
                } else if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
                    auto &comp = cond_expr.Cast<BoundComparisonExpression>();
                    auto left = TransformCondExpr(*comp.left);
                    auto right = TransformCondExpr(*comp.right);
                    return make_uniq_base<Expression, BoundComparisonExpression>(comp.type, std::move(left), std::move(right));
                } else if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
                    auto &conj = cond_expr.Cast<BoundConjunctionExpression>();
                    auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
                    for (auto &child : conj.children) {
                        result->children.push_back(TransformCondExpr(*child));
                    }
                    return std::move(result);
                } else if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
                    auto &op_expr = cond_expr.Cast<BoundOperatorExpression>();
                    auto result = make_uniq<BoundOperatorExpression>(op_expr.type, op_expr.return_type);
                    for (auto &child : op_expr.children) {
                        result->children.push_back(TransformCondExpr(*child));
                    }
                    return std::move(result);
                } else {
                    return cond_expr.Copy();
                }
            };

            // Step 1: Evaluate WHEN condition (if present) to get per-row booleans
            vector<bool> when_mask;
            if (has_when) {
                auto transformed_condition = TransformCondExpr(*constraint->when_condition);
                ExpressionExecutor cond_executor(context);
                cond_executor.AddExpression(*transformed_condition);

                when_mask.reserve(num_rows);

                ColumnDataScanState cond_scan_state;
                gstate.data.InitializeScan(cond_scan_state);
                DataChunk cond_chunk;
                cond_chunk.Initialize(context, gstate.data.Types());

                while (gstate.data.Scan(cond_scan_state, cond_chunk)) {
                    DataChunk cond_result;
                    vector<LogicalType> result_types = {LogicalType::BOOLEAN};
                    cond_result.Initialize(context, result_types);
                    cond_executor.Execute(cond_chunk, cond_result);

                    auto &vec = cond_result.data[0];
                    for (idx_t row_in_chunk = 0; row_in_chunk < cond_chunk.size(); row_in_chunk++) {
                        Value val = vec.GetValue(row_in_chunk);
                        // NULL treated as false: constraint does not apply to this row
                        bool condition_met = val.IsNull() ? false : val.GetValue<bool>();
                        when_mask.push_back(condition_met);
                    }
                }
            }

            // Step 2: Evaluate PER column (if present) to get per-row values
            vector<Value> per_values;
            if (has_per) {
                auto transformed_col = TransformCondExpr(*constraint->per_column);
                ExpressionExecutor per_executor(context);
                per_executor.AddExpression(*transformed_col);

                per_values.reserve(num_rows);

                ColumnDataScanState per_scan_state;
                gstate.data.InitializeScan(per_scan_state);
                DataChunk per_chunk;
                per_chunk.Initialize(context, gstate.data.Types());

                while (gstate.data.Scan(per_scan_state, per_chunk)) {
                    DataChunk per_result;
                    per_result.Initialize(context, {transformed_col->return_type});
                    per_executor.Execute(per_chunk, per_result);

                    auto &vec = per_result.data[0];
                    for (idx_t row_in_chunk = 0; row_in_chunk < per_chunk.size(); row_in_chunk++) {
                        per_values.push_back(vec.GetValue(row_in_chunk));
                    }
                }
            }

            // Step 3: Build unified row_group_ids
            eval_const.row_group_ids.resize(num_rows);

            if (has_per) {
                // PER (with or without WHEN)
                // Map distinct PER values to group IDs (first-seen order)
                unordered_map<string, idx_t> value_to_group;
                idx_t next_group = 0;

                for (idx_t row = 0; row < num_rows; row++) {
                    // WHEN filter: excluded rows get INVALID_INDEX
                    if (has_when && !when_mask[row]) {
                        eval_const.row_group_ids[row] = DConstants::INVALID_INDEX;
                        continue;
                    }
                    // NULL PER values: excluded (matches SQL GROUP BY NULL semantics)
                    if (per_values[row].IsNull()) {
                        eval_const.row_group_ids[row] = DConstants::INVALID_INDEX;
                        continue;
                    }
                    // Assign group ID by PER value
                    string key = per_values[row].ToString();
                    auto it = value_to_group.find(key);
                    if (it == value_to_group.end()) {
                        value_to_group[key] = next_group;
                        eval_const.row_group_ids[row] = next_group;
                        next_group++;
                    } else {
                        eval_const.row_group_ids[row] = it->second;
                    }
                }
                eval_const.num_groups = next_group;
            } else {
                // WHEN only (no PER): one group (group 0) for matching rows
                for (idx_t row = 0; row < num_rows; row++) {
                    eval_const.row_group_ids[row] = when_mask[row] ? 0 : DConstants::INVALID_INDEX;
                }
                eval_const.num_groups = 1;
            }
        }

        gstate.evaluated_constraints.push_back(std::move(eval_const));
    }

    // 2. Evaluate objective
    if (gstate.objective) {
        // Transform expressions (same as for constraints)
        std::function<unique_ptr<Expression>(const Expression&)> TransformExpression;
        TransformExpression = [&](const Expression &expr) -> unique_ptr<Expression> {
            if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                auto &colref = expr.Cast<BoundColumnRefExpression>();
                return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
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
                return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function, std::move(new_children), std::move(new_bind_info));
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

                    // Check for NULL values
                    if (val.IsNull()) {
                        throw InvalidInputException(
                            "DECIDE objective coefficient returned NULL at row %llu. "
                            "NULL values are not allowed in optimization objective. "
                            "Use COALESCE() to handle NULLs or filter them with WHERE clause.",
                            gstate.evaluated_objective_coefficients[term_idx].size());
                    }

                    double double_val = val.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();

                    // Check for NaN or Infinity
                    if (!std::isfinite(double_val)) {
                        throw InvalidInputException(
                            "DECIDE objective coefficient contains invalid value (NaN or Infinity) at row %llu. "
                            "Common causes:\n"
                            "  • Division by zero in objective expression\n"
                            "  • Arithmetic overflow in calculations\n"
                            "  • NULL values that propagated through math operations\n"
                            "Check your objective expressions and input data.",
                            gstate.evaluated_objective_coefficients[term_idx].size());
                    }

                    gstate.evaluated_objective_coefficients[term_idx].push_back(double_val);
                }
            }
        }

        // PackDB: Apply WHEN mask to objective coefficients
        if (gstate.objective->when_condition) {
            std::function<unique_ptr<Expression>(const Expression&)> TransformCondExpr;
            TransformCondExpr = [&](const Expression &cond_expr) -> unique_ptr<Expression> {
                if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    auto &colref = cond_expr.Cast<BoundColumnRefExpression>();
                    return make_uniq_base<Expression, BoundReferenceExpression>(colref.return_type, colref.binding.column_index);
                } else if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
                    auto &func = cond_expr.Cast<BoundFunctionExpression>();
                    vector<unique_ptr<Expression>> new_children;
                    for (auto &child : func.children) {
                        new_children.push_back(TransformCondExpr(*child));
                    }
                    unique_ptr<FunctionData> new_bind_info;
                    if (func.bind_info) {
                        new_bind_info = func.bind_info->Copy();
                    }
                    return make_uniq_base<Expression, BoundFunctionExpression>(func.return_type, func.function, std::move(new_children), std::move(new_bind_info));
                } else if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    auto &cast = cond_expr.Cast<BoundCastExpression>();
                    auto transformed_child = TransformCondExpr(*cast.child);
                    return BoundCastExpression::AddCastToType(context, std::move(transformed_child), cast.return_type, cast.try_cast);
                } else if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
                    auto &comp = cond_expr.Cast<BoundComparisonExpression>();
                    auto left = TransformCondExpr(*comp.left);
                    auto right = TransformCondExpr(*comp.right);
                    return make_uniq_base<Expression, BoundComparisonExpression>(comp.type, std::move(left), std::move(right));
                } else if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
                    auto &conj = cond_expr.Cast<BoundConjunctionExpression>();
                    auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
                    for (auto &child : conj.children) {
                        result->children.push_back(TransformCondExpr(*child));
                    }
                    return std::move(result);
                } else if (cond_expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
                    auto &op_expr = cond_expr.Cast<BoundOperatorExpression>();
                    auto result = make_uniq<BoundOperatorExpression>(op_expr.type, op_expr.return_type);
                    for (auto &child : op_expr.children) {
                        result->children.push_back(TransformCondExpr(*child));
                    }
                    return std::move(result);
                } else {
                    return cond_expr.Copy();
                }
            };

            auto transformed_condition = TransformCondExpr(*gstate.objective->when_condition);

            ExpressionExecutor cond_executor(context);
            cond_executor.AddExpression(*transformed_condition);

            ColumnDataScanState obj_cond_scan_state;
            gstate.data.InitializeScan(obj_cond_scan_state);
            DataChunk obj_cond_chunk;
            obj_cond_chunk.Initialize(context, gstate.data.Types());

            idx_t row_offset = 0;
            while (gstate.data.Scan(obj_cond_scan_state, obj_cond_chunk)) {
                DataChunk cond_result;
                vector<LogicalType> result_types = {LogicalType::BOOLEAN};
                cond_result.Initialize(context, result_types);
                cond_executor.Execute(obj_cond_chunk, cond_result);

                auto &vec = cond_result.data[0];
                for (idx_t row_in_chunk = 0; row_in_chunk < obj_cond_chunk.size(); row_in_chunk++) {
                    Value val = vec.GetValue(row_in_chunk);
                    bool condition_met = val.IsNull() ? false : val.GetValue<bool>();
                    if (!condition_met) {
                        // Zero out all objective coefficients for this row
                        for (idx_t term_idx = 0; term_idx < gstate.evaluated_objective_coefficients.size(); term_idx++) {
                            gstate.evaluated_objective_coefficients[term_idx][row_offset + row_in_chunk] = 0.0;
                        }
                    }
                }
                row_offset += obj_cond_chunk.size();
            }
        }

        // No extra debug here; solver output will show timings/objective
    }

    //===--------------------------------------------------------------------===//
    // PHASE 3: Build and Solve ILP with HiGHS
    //===--------------------------------------------------------------------===//

    //===--------------------------------------------------------------------===//
    // PHASE 3: Build and Solve ILP
    //===--------------------------------------------------------------------===//

    // Construct SolverInput (num_decide_vars already declared above)
    SolverInput solver_input;
    solver_input.num_rows = num_rows;
    solver_input.num_decide_vars = num_decide_vars;
    
    // Variable types and bounds
    solver_input.variable_types.resize(num_decide_vars);
    solver_input.lower_bounds.assign(num_decide_vars, 0.0); // Default lower
    solver_input.upper_bounds.assign(num_decide_vars, 1e30); // Default upper
    
    for (idx_t var = 0; var < num_decide_vars; var++) {
        auto &decide_var = decide_variables[var]->Cast<BoundColumnRefExpression>();
        solver_input.variable_types[var] = decide_var.return_type;
        
        // Set default bounds based on type (same logic as in solver, but good to be explicit)
        if (decide_var.return_type == LogicalType::BOOLEAN) {
            solver_input.upper_bounds[var] = 1.0;
        }
    }
    
    // Extract bounds from constraints
    gstate.ExtractVariableBounds(solver_input.lower_bounds, solver_input.upper_bounds);
    
    // Constraints
    solver_input.constraints = std::move(gstate.evaluated_constraints);
    
    // Objective
    solver_input.objective_coefficients = std::move(gstate.evaluated_objective_coefficients);
    solver_input.objective_variable_indices = std::move(gstate.objective_variable_indices);
    solver_input.sense = decide_sense;
    
    gstate.ilp_solution = SolveILP(solver_input);

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
    return make_uniq_base<GlobalSourceState, DecideGlobalSourceState>(*this, sink);
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