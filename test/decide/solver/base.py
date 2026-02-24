"""Abstract base class for oracle solvers."""

from __future__ import annotations

from abc import ABC, abstractmethod

from .types import ObjSense, SolverResult, VarType


class OracleSolver(ABC):
    """Abstract interface for ILP oracle solvers.

    Each backend (Gurobi, HiGHS) implements this interface so that test
    oracles are solver-agnostic.
    """

    @abstractmethod
    def create_model(self, name: str) -> None:
        """Create a new empty model, discarding any previous one."""

    @abstractmethod
    def add_variable(
        self,
        name: str,
        var_type: VarType = VarType.BINARY,
        lb: float = 0.0,
        ub: float | None = None,
    ) -> None:
        """Add a decision variable to the model."""

    @abstractmethod
    def add_constraint(
        self,
        coeffs: dict[str, float],
        sense: str,
        rhs: float,
        name: str = "",
    ) -> None:
        """Add a linear constraint.

        Args:
            coeffs: Mapping of variable name to coefficient.
            sense: One of '<=', '>=', '='.
            rhs: Right-hand side value.
            name: Optional constraint name.
        """

    @abstractmethod
    def set_objective(
        self,
        coeffs: dict[str, float],
        sense: ObjSense,
    ) -> None:
        """Set the linear objective function."""

    @abstractmethod
    def solve(self, time_limit: float = 60.0) -> SolverResult:
        """Solve the model and return the result."""

    @abstractmethod
    def solver_name(self) -> str:
        """Return the human-readable solver backend name."""
