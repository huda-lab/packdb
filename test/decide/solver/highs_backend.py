"""HiGHS backend for the oracle solver."""

from __future__ import annotations

import math
import time

from .base import OracleSolver
from .types import ObjSense, SolverResult, SolverStatus, VarType


class HighsSolver(OracleSolver):
    """Oracle solver backed by highspy."""

    def __init__(self) -> None:
        import highspy
        self._highspy = highspy
        self._h = None
        self._var_names: list[str] = []
        self._var_index: dict[str, int] = {}

    def create_model(self, name: str) -> None:
        h = self._highspy.Highs()
        h.silent()
        self._h = h
        self._var_names = []
        self._var_index = {}

    def add_variable(
        self,
        name: str,
        var_type: VarType = VarType.BINARY,
        lb: float = 0.0,
        ub: float | None = None,
    ) -> None:
        highspy = self._highspy
        if ub is None:
            ub = 1.0 if var_type == VarType.BINARY else highspy.kHighsInf

        idx = len(self._var_names)
        self._var_names.append(name)
        self._var_index[name] = idx

        # Add column: cost=0, lb, ub, num_entries=0, no indices/values
        self._h.addCol(0.0, lb, ub, 0, [], [])

        # Set integrality
        type_map = {
            VarType.BINARY: highspy.HighsVarType.kInteger,
            VarType.INTEGER: highspy.HighsVarType.kInteger,
            VarType.CONTINUOUS: highspy.HighsVarType.kContinuous,
        }
        self._h.changeColIntegrality(idx, type_map[var_type])

    def add_constraint(
        self,
        coeffs: dict[str, float],
        sense: str,
        rhs: float,
        name: str = "",
    ) -> None:
        highspy = self._highspy
        indices = []
        values = []
        for vname, coeff in coeffs.items():
            indices.append(self._var_index[vname])
            values.append(coeff)

        if sense == "<=":
            lower = -highspy.kHighsInf
            upper = rhs
        elif sense == ">=":
            lower = rhs
            upper = highspy.kHighsInf
        elif sense == "=":
            lower = rhs
            upper = rhs
        else:
            raise ValueError(f"Unknown constraint sense: {sense!r}")

        self._h.addRow(lower, upper, len(indices), indices, values)

    def set_objective(
        self,
        coeffs: dict[str, float],
        sense: ObjSense,
    ) -> None:
        highspy = self._highspy
        for vname, coeff in coeffs.items():
            self._h.changeColCost(self._var_index[vname], coeff)

        obj_sense = (
            highspy.ObjSense.kMaximize
            if sense == ObjSense.MAXIMIZE
            else highspy.ObjSense.kMinimize
        )
        self._h.changeObjectiveSense(obj_sense)

    def solve(self, time_limit: float = 60.0) -> SolverResult:
        highspy = self._highspy
        self._h.setOptionValue("time_limit", time_limit)

        t0 = time.perf_counter()
        self._h.run()
        solve_time = time.perf_counter() - t0

        model_status = self._h.getInfoValue("primal_solution_status")
        info = self._h.getInfoValue("objective_function_value")

        # Map HiGHS model status
        hms = self._h.getModelStatus()
        status_map = {
            highspy.HighsModelStatus.kOptimal: SolverStatus.OPTIMAL,
            highspy.HighsModelStatus.kInfeasible: SolverStatus.INFEASIBLE,
            highspy.HighsModelStatus.kUnbounded: SolverStatus.UNBOUNDED,
        }
        solver_status = status_map.get(hms, SolverStatus.ERROR)

        obj_val = None
        var_vals = {}
        if solver_status == SolverStatus.OPTIMAL:
            sol = self._h.getSolution()
            obj_val = self._h.getInfoValue("objective_function_value")
            # getInfoValue returns (status, value) tuple
            if isinstance(obj_val, tuple):
                obj_val = obj_val[1]
            col_values = sol.col_value
            var_vals = {
                self._var_names[i]: col_values[i]
                for i in range(len(self._var_names))
            }

        return SolverResult(
            status=solver_status,
            objective_value=obj_val,
            variable_values=var_vals,
            solve_time_seconds=solve_time,
        )

    def solver_name(self) -> str:
        return "HiGHS"
