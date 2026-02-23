# Adaptive Processing — Implemented Features

No adaptive processing features are currently implemented.

**Note**: Previous documentation incorrectly stated that a solver time limit (default 60 seconds) was implemented. This has been verified as inaccurate — neither the Gurobi backend (`gurobi_solver.cpp`) nor the HiGHS backend (`deterministic_naive.cpp`) configure a timeout. Both solvers run with their library defaults (no explicit time bound).
