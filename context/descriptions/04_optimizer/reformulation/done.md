# Reformulation — Implemented Features

No reformulation techniques are currently implemented. Constraints and objectives are passed to the solver without algebraic transformation.

The system relies entirely on the solver's internal preprocessing (both Gurobi and HiGHS perform their own presolve passes).
