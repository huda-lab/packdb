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
    def set_quadratic_objective(
        self,
        linear: dict[str, float],
        quadratic: dict[tuple[str, str], float],
        sense: ObjSense,
        constant: float = 0.0,
    ) -> None:
        """Set a quadratic objective.

        Args:
            linear: Linear coefficients {var_name: coeff}.
            quadratic: Q terms {(var_i, var_j): coeff} for coeff * x_i * x_j.
                Diagonal entries (i == j) produce x_i^2; off-diagonal entries
                produce bilinear x_i * x_j. Symmetric entries should not be
                duplicated — (a, b) and (b, a) both refer to the same term
                and the backend will de-duplicate.
            sense: MAXIMIZE or MINIMIZE.
            constant: Objective constant (added to the final value, unaffected
                by optimization).
        """

    @abstractmethod
    def add_quadratic_constraint(
        self,
        linear: dict[str, float],
        quadratic: dict[tuple[str, str], float],
        sense: str,
        rhs: float,
        name: str = "",
    ) -> None:
        """Add a quadratic constraint: linear_expr + sum(Q_ij x_i x_j) [sense] rhs."""

    @abstractmethod
    def add_indicator_constraint(
        self,
        indicator_var: str,
        indicator_val: int,
        coeffs: dict[str, float],
        sense: str,
        rhs: float,
        name: str = "",
    ) -> None:
        """Add a logical implication: if ``indicator_var == indicator_val``
        then ``sum(coeffs) [sense] rhs``.

        ``indicator_var`` must be a binary variable already added via
        ``add_variable``. ``indicator_val`` is 0 or 1. This maps to Gurobi's
        native ``addGenConstrIndicator`` so the encoding is Big-M-free and
        independent of any hand-picked bound."""

    @abstractmethod
    def solve(self, time_limit: float = 60.0) -> SolverResult:
        """Solve the model and return the result."""

    @abstractmethod
    def solver_name(self) -> str:
        """Return the human-readable solver backend name."""
