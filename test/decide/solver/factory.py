"""Auto-detect the best available solver backend."""

from __future__ import annotations

from .base import OracleSolver


def get_solver() -> OracleSolver:
    """Return the best available oracle solver.

    Tries gurobipy first (commercial, faster), falls back to highspy (bundled).
    Raises ImportError if neither is available.
    """
    try:
        import gurobipy  # noqa: F401
        from .gurobi_backend import GurobiSolver
        return GurobiSolver()
    except (ImportError, Exception):
        pass

    try:
        import highspy  # noqa: F401
        from .highs_backend import HighsSolver
        return HighsSolver()
    except ImportError:
        pass

    raise ImportError(
        "No ILP solver available. Install highspy (pip install highspy) "
        "or gurobipy (requires Gurobi license)."
    )
