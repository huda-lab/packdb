Complete Physical Layer + Solver Integration Plan
🎯 GOAL: End-to-end working DECIDE execution with HiGHS solver
📊 CURRENT STATUS
✅ What Works:
HiGHS v1.11.0 already integrated (CMake FetchContent, linked to duckdb_packdb)
Parser, binder, symbolic normalization, logical planning all complete
Data collection (sink) infrastructure working
Physical operator skeleton exists
❌ What's Broken/Stubbed:
Analyzer Methods: Expect simple SUM(x*coef), binder produces SUM(3*x*l_tax + 2*y*l_discount + const)
Finalize(): Returns dummy data i % 100
GetData(): Returns hardcoded values (10, 11, 12...)
No Solver Usage: HiGHS library linked but never called
🚀 IMPLEMENTATION PLAN
PHASE 1: Fix Physical Layer Analyzers (3-5 days)
CRITICAL BLOCKER - Must complete before solver work
Why This Is Required:
The binder produces expressions like:
SUM(5*x*(l_tax + l_discount) + y*(2*l_quantity - 3*l_extendedprice) + 11)
But the current analyzer expects:
SUM(x * l_tax)
The current AnalyzeSumArgument() will crash trying to cast the + operator as a multiplication.
Files to Modify:
src/include/duckdb/execution/operator/decide/physical_decide.hpp
src/execution/operator/decide/physical_decide.cpp
Step 1.1: Define New Data Structures
Add to physical_decide.hpp:
struct LinearTerm {
    idx_t variable_index;              // -1 for constants, 0+ for DECIDE vars
    unique_ptr<Expression> coefficient; // Row-varying expression (e.g., 3*l_tax)
};

struct LinearConstraint {
    vector<LinearTerm> lhs_terms;       // All additive terms from LHS
    unique_ptr<Expression> rhs_expr;    // RHS (may contain aggregates)
    ExpressionType comparison_type;     // COMPARE_LESSTHANOREQUALTO or GREATERTHANOREQUALTO
};

struct LinearObjective {
    vector<LinearTerm> terms;           // All objective terms
};
Update DecideGlobalSinkState:
// REPLACE these:
// vector<unique_ptr<DeterministicConstraint>> cons;
// unique_ptr<DeterministicObjective> obj;

// WITH:
vector<unique_ptr<LinearConstraint>> constraints;
unique_ptr<LinearObjective> objective;
Step 1.2: Implement Expression Visitor Functions
Add to physical_decide.cpp:
// Find DECIDE variable in expression tree
idx_t PhysicalDecide::FindDecideVariable(const Expression &expr) const {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        for (idx_t i = 0; i < decide_variables.size(); i++) {
            auto &decide_var = decide_variables[i]->Cast<BoundColumnRefExpression>();
            if (colref.binding == decide_var.binding) {
                return i;
            }
        }
    }
    
    // Recurse into children using ExpressionIterator
    idx_t result = DConstants::INVALID_INDEX;
    ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) {
        if (result == DConstants::INVALID_INDEX) {
            result = FindDecideVariable(*child);
        }
    });
    return result;
}

// Check if expression contains a specific variable
bool PhysicalDecide::ContainsVariable(const Expression &expr, idx_t var_idx) const {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        auto &decide_var = decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
        return colref.binding == decide_var.binding;
    }
    
    bool found = false;
    ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) {
        if (!found && ContainsVariable(*child, var_idx)) {
            found = true;
        }
    });
    return found;
}

// Extract coefficient excluding the variable
unique_ptr<Expression> PhysicalDecide::ExtractCoefficientWithoutVariable(
    const Expression &expr, idx_t var_idx) const {
    
    // If this IS the variable, return constant 1
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = expr.Cast<BoundColumnRefExpression>();
        auto &decide_var = decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
        if (colref.binding == decide_var.binding) {
            return make_uniq<BoundConstantExpression>(Value::INTEGER(1));
        }
    }
    
    // If it's a multiplication, remove the variable child
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
            auto new_func = make_uniq<BoundFunctionExpression>(
                func.return_type, func.function, std::move(filtered_children), nullptr);
            return std::move(new_func);
        }
    }
    
    // Otherwise, return copy of entire expression
    return expr.Copy();
}

// Main visitor: extract all linear terms from SUM argument
void PhysicalDecide::ExtractLinearTerms(const Expression &expr, 
                                       vector<LinearTerm> &out_terms) const {
    if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = expr.Cast<BoundFunctionExpression>();
        
        // Addition: recurse on all children
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
                // No variable - constant term
                out_terms.push_back(LinearTerm{DConstants::INVALID_INDEX, func.Copy()});
            } else {
                // Has variable - extract coefficient
                auto coef = ExtractCoefficientWithoutVariable(func, var_idx);
                out_terms.push_back(LinearTerm{var_idx, std::move(coef)});
            }
            return;
        }
    }
    
    // Constant or column reference
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
Step 1.3: Rewrite AnalyzeConstraint()
Replace in DecideGlobalSinkState constructor:
void DecideGlobalSinkState::AnalyzeConstraint(const unique_ptr<Expression>& expr_ptr) {
    auto &expr = *expr_ptr;
    
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::BOUND_CONJUNCTION: {
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
        throw InternalException("Unexpected expression in DECIDE constraints");
    }
}
Step 1.4: Rewrite AnalyzeObjective()
Replace in DecideGlobalSinkState constructor:
void DecideGlobalSinkState::AnalyzeObjective(const unique_ptr<Expression>& expr_ptr) {
    auto &expr = *expr_ptr;
    
    if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
        throw InternalException("Objective must be a SUM aggregate");
    }
    
    auto &agg = expr.Cast<BoundAggregateExpression>();
    objective = make_uniq<LinearObjective>();
    op.ExtractLinearTerms(*agg.children[0], objective->terms);
}
Step 1.5: Remove Old Code
Delete from physical_decide.cpp:
AnalyzeSumArgument() method (lines 80-88)
DeterministicConstraint struct (lines 33-38)
DeterministicObjective struct (lines 40-45)
PHASE 2: Evaluate Coefficient Expressions (2-3 days)
Why This Is Required:
Coefficients like 3*l_tax + l_discount vary per row. We need to evaluate them on actual data to build the ILP model.
Step 2.1: Add Coefficient Evaluation Structure
Add to DecideGlobalSinkState:
struct EvaluatedConstraint {
    vector<idx_t> variable_indices;           // Which variable for each term
    vector<vector<double>> row_coefficients;  // [term_idx][row_idx] = coefficient value
    vector<double> rhs_values;                // [row_idx] = RHS value
    ExpressionType comparison_type;
};

vector<EvaluatedConstraint> evaluated_constraints;
vector<vector<double>> evaluated_objective_coefficients;  // [term_idx][row_idx]
vector<idx_t> objective_variable_indices;
Step 2.2: Implement Evaluation in Finalize()
Replace Finalize() method:
SinkFinalizeType PhysicalDecide::Finalize(Pipeline &pipeline, Event &event,
                                          ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
    auto &gstate = input.global_state.Cast<DecideGlobalSinkState>();
    idx_t num_rows = gstate.data.Count();
    
    // --- EVALUATE COEFFICIENT EXPRESSIONS ---
    
    // 1. Evaluate constraints
    for (auto &constraint : gstate.constraints) {
        EvaluatedConstraint eval_const;
        eval_const.comparison_type = constraint->comparison_type;
        
        // Prepare expression executor for LHS terms
        ExpressionExecutor lhs_executor(context);
        for (auto &term : constraint->lhs_terms) {
            eval_const.variable_indices.push_back(term.variable_index);
            lhs_executor.AddExpression(*term.coefficient);
        }
        
        // Prepare executor for RHS
        ExpressionExecutor rhs_executor(context);
        rhs_executor.AddExpression(*constraint->rhs_expr);
        
        // Scan data and evaluate
        ColumnDataScanState scan_state;
        gstate.data.InitializeScan(scan_state);
        
        DataChunk chunk;
        chunk.Initialize(context, gstate.data.Types());
        
        eval_const.row_coefficients.resize(constraint->lhs_terms.size());
        
        while (gstate.data.Scan(scan_state, chunk)) {
            // Evaluate LHS coefficients
            DataChunk lhs_result;
            lhs_executor.Execute(chunk, lhs_result);
            
            for (idx_t term_idx = 0; term_idx < lhs_result.ColumnCount(); term_idx++) {
                auto &vec = lhs_result.data[term_idx];
                for (idx_t row_in_chunk = 0; row_in_chunk < chunk.size(); row_in_chunk++) {
                    double val = vec.GetValue(row_in_chunk).GetValue<double>();
                    eval_const.row_coefficients[term_idx].push_back(val);
                }
            }
            
            // Evaluate RHS
            DataChunk rhs_result;
            rhs_executor.Execute(chunk, rhs_result);
            auto &rhs_vec = rhs_result.data[0];
            for (idx_t row_in_chunk = 0; row_in_chunk < chunk.size(); row_in_chunk++) {
                double val = rhs_vec.GetValue(row_in_chunk).GetValue<double>();
                eval_const.rhs_values.push_back(val);
            }
        }
        
        gstate.evaluated_constraints.push_back(std::move(eval_const));
    }
    
    // 2. Evaluate objective
    ExpressionExecutor obj_executor(context);
    for (auto &term : gstate.objective->terms) {
        gstate.objective_variable_indices.push_back(term.variable_index);
        obj_executor.AddExpression(*term.coefficient);
    }
    
    ColumnDataScanState obj_scan_state;
    gstate.data.InitializeScan(obj_scan_state);
    
    DataChunk obj_chunk;
    obj_chunk.Initialize(context, gstate.data.Types());
    
    gstate.evaluated_objective_coefficients.resize(gstate.objective->terms.size());
    
    while (gstate.data.Scan(obj_scan_state, obj_chunk)) {
        DataChunk obj_result;
        obj_executor.Execute(obj_chunk, obj_result);
        
        for (idx_t term_idx = 0; term_idx < obj_result.ColumnCount(); term_idx++) {
            auto &vec = obj_result.data[term_idx];
            for (idx_t row_in_chunk = 0; row_in_chunk < obj_chunk.size(); row_in_chunk++) {
                double val = vec.GetValue(row_in_chunk).GetValue<double>();
                gstate.evaluated_objective_coefficients[term_idx].push_back(val);
            }
        }
    }
    
    // --- BUILD AND SOLVE ILP (Phase 3) ---
    SolveILP(gstate, context, num_rows);
    
    return SinkFinalizeType::READY;
}
PHASE 3: HiGHS Solver Integration (4-6 days)
Step 3.1: Add HiGHS Include
Add to physical_decide.cpp:
#include "Highs.h"
Step 3.2: Implement SolveILP() Method
Add to physical_decide.cpp:
void PhysicalDecide::SolveILP(DecideGlobalSinkState &gstate, 
                             ClientContext &context, 
                             idx_t num_rows) const {
    
    Highs highs;
    highs.setOptionValue("log_to_console", false);
    
    idx_t num_decide_vars = decide_variables.size();
    idx_t total_vars = num_rows * num_decide_vars;
    
    // --- ADD VARIABLES ---
    vector<double> lower_bounds(total_vars, 0.0);
    vector<double> upper_bounds(total_vars, HIGHS_CONST_INF);
    
    highs.addCols(total_vars, nullptr, lower_bounds.data(), upper_bounds.data(),
                  0, nullptr, nullptr, nullptr);
    
    // Set variable types (INTEGER, BINARY, CONTINUOUS)
    for (idx_t var_idx = 0; var_idx < num_decide_vars; var_idx++) {
        auto &var_type = decide_variables[var_idx]->return_type;
        HighsVarType highs_type = HighsVarType::kContinuous;
        
        if (var_type == LogicalType::INTEGER) {
            highs_type = HighsVarType::kInteger;
        } else if (var_type == LogicalType::BOOLEAN) {
            highs_type = HighsVarType::kInteger;
            // Binary: set upper bound to 1
            for (idx_t row = 0; row < num_rows; row++) {
                idx_t col_idx = row * num_decide_vars + var_idx;
                upper_bounds[col_idx] = 1.0;
            }
        }
        
        if (highs_type != HighsVarType::kContinuous) {
            for (idx_t row = 0; row < num_rows; row++) {
                idx_t col_idx = row * num_decide_vars + var_idx;
                highs.changeColIntegrality(col_idx, highs_type);
            }
        }
    }
    
    // --- ADD CONSTRAINTS ---
    for (auto &eval_const : gstate.evaluated_constraints) {
        for (idx_t row = 0; row < num_rows; row++) {
            vector<idx_t> indices;
            vector<double> values;
            
            for (idx_t term_idx = 0; term_idx < eval_const.variable_indices.size(); term_idx++) {
                idx_t var_idx = eval_const.variable_indices[term_idx];
                
                if (var_idx == DConstants::INVALID_INDEX) {
                    // Constant term - add to RHS adjustment
                    continue;
                }
                
                idx_t col_idx = row * num_decide_vars + var_idx;
                double coef = eval_const.row_coefficients[term_idx][row];
                
                indices.push_back(col_idx);
                values.push_back(coef);
            }
            
            double rhs = eval_const.rhs_values[row];
            double constraint_lb = -HIGHS_CONST_INF;
            double constraint_ub = HIGHS_CONST_INF;
            
            if (eval_const.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
                constraint_ub = rhs;
            } else if (eval_const.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
                constraint_lb = rhs;
            }
            
            highs.addRow(constraint_lb, constraint_ub, 
                        indices.size(), indices.data(), values.data());
        }
    }
    
    // --- SET OBJECTIVE ---
    vector<double> obj_coeffs(total_vars, 0.0);
    
    for (idx_t row = 0; row < num_rows; row++) {
        for (idx_t term_idx = 0; term_idx < gstate.objective_variable_indices.size(); term_idx++) {
            idx_t var_idx = gstate.objective_variable_indices[term_idx];
            
            if (var_idx == DConstants::INVALID_INDEX) {
                continue;  // Constant terms don't affect optimal solution
            }
            
            idx_t col_idx = row * num_decide_vars + var_idx;
            double coef = gstate.evaluated_objective_coefficients[term_idx][row];
            obj_coeffs[col_idx] = coef;
        }
    }
    
    highs.changeColsCost(total_vars, nullptr, obj_coeffs.data());
    
    ObjSense sense = (decide_sense == DecideSense::MAXIMIZE) ? 
                     ObjSense::kMaximize : ObjSense::kMinimize;
    highs.changeObjectiveSense(sense);
    
    // --- SOLVE ---
    HighsStatus status = highs.run();
    
    if (status != HighsStatus::kOk) {
        throw InternalException("HiGHS solver failed with status %d", (int)status);
    }
    
    const HighsSolution& solution = highs.getSolution();
    
    if (solution.col_value.empty()) {
        throw InternalException("HiGHS returned empty solution");
    }
    
    // --- STORE SOLUTION ---
    gstate.ilp_solution.assign(solution.col_value.begin(), solution.col_value.end());
}
PHASE 4: Return Solution Values (1-2 days)
Step 4.1: Update GetData()
Replace GetData() method:
SourceResultType PhysicalDecide::GetData(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSourceInput &input) const {
    auto &gstate = sink_state->Cast<DecideGlobalSinkState>();
    auto &source_state = input.global_state.Cast<DecideGlobalSourceState>();
    
    // Scan buffered data
    gstate.data.Scan(source_state.scan_state, chunk);
    if (chunk.size() == 0) {
        return SourceResultType::FINISHED;
    }
    
    // Add DECIDE variable columns
    idx_t base_row_idx = source_state.current_row_offset;
    idx_t num_decide_vars = decide_variables.size();
    
    for (idx_t var_idx = 0; var_idx < num_decide_vars; var_idx++) {
        auto &output_vector = chunk.data[types.size() - num_decide_vars + var_idx];
        auto &var_type = decide_variables[var_idx]->return_type;
        
        // Extract solution values for this variable across rows in chunk
        for (idx_t row_in_chunk = 0; row_in_chunk < chunk.size(); row_in_chunk++) {
            idx_t global_row_idx = base_row_idx + row_in_chunk;
            idx_t solution_idx = global_row_idx * num_decide_vars + var_idx;
            
            double solution_value = gstate.ilp_solution[solution_idx];
            
            if (var_type == LogicalType::INTEGER || var_type == LogicalType::BOOLEAN) {
                output_vector.SetValue(row_in_chunk, 
                    Value::INTEGER(static_cast<int64_t>(std::round(solution_value))));
            } else {
                output_vector.SetValue(row_in_chunk, Value::DOUBLE(solution_value));
            }
        }
    }
    
    source_state.current_row_offset += chunk.size();
    return SourceResultType::HAVE_MORE_OUTPUT;
}
Step 4.2: Update DecideGlobalSourceState
Add to physical_decide.hpp:
struct DecideGlobalSourceState : public GlobalSourceState {
    DecideGlobalSourceState(ClientContext &context, const PhysicalDecide &op);
    
    ColumnDataScanState scan_state;
    idx_t current_row_offset = 0;  // ADD THIS
};
PHASE 5: Testing & Debugging (2-3 days)
Step 5.1: Add Debug Output
In Finalize(), add:
deb("=== DECIDE Analysis Results ===");
deb("Number of rows:", num_rows);
deb("Number of DECIDE variables:", decide_variables.size());
deb("Number of constraints:", gstate.constraints.size());

for (idx_t i = 0; i < gstate.evaluated_constraints.size(); i++) {
    auto &ec = gstate.evaluated_constraints[i];
    deb("Constraint", i, "has", ec.variable_indices.size(), "terms");
    for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
        if (ec.variable_indices[t] != DConstants::INVALID_INDEX) {
            deb("  Term", t, ": var", ec.variable_indices[t], 
                "sample coef (row 0):", ec.row_coefficients[t][0]);
        }
    }
}

deb("Objective has", gstate.objective_variable_indices.size(), "terms");
deb("ILP has", total_vars, "total variables");
Step 5.2: Test Queries
Run in order:
Simplest case:
SELECT SUM(x) FROM lineitem DECIDE x 
SUCH THAT SUM(x) <= 10 
MAXIMIZE SUM(x) LIMIT 5;
With row coefficient:
SELECT SUM(x) FROM lineitem DECIDE x 
SUCH THAT SUM(x*l_tax) <= 100 
MAXIMIZE SUM(x*l_quantity) LIMIT 5;
Multi-variable:
SELECT SUM(x), SUM(y) FROM lineitem DECIDE x, y 
SUCH THAT SUM(5*x*(l_tax + l_discount) + y*(2*l_quantity)) >= 10 
MAXIMIZE SUM(6*x*l_extendedprice + 4*y*l_discount) LIMIT 5;
Step 5.3: Validation
For each test query:
Check no crashes
Verify solution values are reasonable
Manually verify constraints are satisfied (spot check)
Check objective value matches expectations
📁 FILES TO MODIFY
Primary Files:
src/include/duckdb/execution/operator/decide/physical_decide.hpp
Add LinearTerm, LinearConstraint, LinearObjective structs
Add EvaluatedConstraint struct
Update DecideGlobalSinkState members
Add helper method declarations
Update DecideGlobalSourceState
src/execution/operator/decide/physical_decide.cpp
Add #include "Highs.h"
Implement visitor functions (FindDecideVariable, ExtractLinearTerms, etc.)
Rewrite AnalyzeConstraint() and AnalyzeObjective()
Rewrite Finalize() with coefficient evaluation
Add SolveILP() method
Rewrite GetData() to use real solution
Secondary Files (for testing):
test/packdb/test.sql
Uncomment/add test queries progressively
⏱️ ESTIMATED TIMELINE
Phase	Effort	Complexity	Risk
Phase 1: Fix Analyzers	3-5 days	Medium-High	Low
Phase 2: Evaluate Coefficients	2-3 days	Medium	Low
Phase 3: HiGHS Integration	4-6 days	High	Medium
Phase 4: Return Solutions	1-2 days	Low	Low
Phase 5: Testing	2-3 days	Medium	Low
TOTAL	12-19 days	-	-
🎯 SUCCESS CRITERIA
✅ Phase 1 Complete:
No crashes when analyzing complex constraints
Debug output shows correct term extraction
Multiple variables per constraint supported
✅ Phase 2 Complete:
Coefficients evaluate correctly on test data
RHS values computed properly
No type errors during evaluation
✅ Phase 3 Complete:
HiGHS solver called successfully
Solution returned without errors
Variable types (INTEGER/BINARY/REAL) respected
✅ Phase 4 Complete:
GetData() returns actual solution values
Values match variable types
Correct mapping of solution to rows
✅ Phase 5 Complete:
All test queries execute without crashes
Solutions satisfy constraints (manual verification)
Objective values are optimal (spot check)
🚨 CRITICAL DEPENDENCIES
Phase 2 depends on Phase 1 - Cannot evaluate until terms are extracted
Phase 3 depends on Phase 2 - Cannot solve until coefficients are evaluated
Phase 4 depends on Phase 3 - Cannot return results until solution exists
Must proceed sequentially through phases!
💡 KEY INSIGHTS
HiGHS is already integrated - No build system changes needed
Physical layer analyzer is the bottleneck - Must fix before anything else works
Expression evaluation is standard - Use existing ExpressionExecutor pattern
ILP formulation is per-row - Each row gets its own set of variables
Solution is flat array - Index as row * num_vars + var_idx