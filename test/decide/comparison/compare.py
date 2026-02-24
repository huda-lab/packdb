"""Comparison logic for PackDB DECIDE results vs oracle solver results.

Key design decision: we compare **objective values only**, not variable
assignments.  ILP problems frequently have multiple optimal solutions with
the same objective but different variable values.
"""

from __future__ import annotations

import contextlib
from typing import Callable

import pytest

from solver.types import SolverResult, SolverStatus


def compute_packdb_objective(
    packdb_rows: list[tuple],
    packdb_cols: list[str],
    decide_var_names: list[str],
    coeff_fn: Callable[[tuple], dict[str, float]],
) -> float:
    """Compute the achieved objective from PackDB output rows.

    Args:
        packdb_rows: Rows returned by packdb_conn.execute(...).fetchall().
        packdb_cols: Column names from packdb_conn.description.
        decide_var_names: Names of the DECIDE variable columns (e.g. ["x"]).
        coeff_fn: Given a row tuple, returns {var_name: coefficient} for the
            objective.  For a simple ``MAXIMIZE SUM(x * col)``, this would
            return ``{"x": row[col_index]}``.

    Returns:
        The sum of (variable_value * coefficient) across all rows.
    """
    col_idx = {name: i for i, name in enumerate(packdb_cols)}
    total = 0.0
    for row in packdb_rows:
        coeffs = coeff_fn(row)
        for vname in decide_var_names:
            var_val = float(row[col_idx[vname]])
            total += var_val * coeffs.get(vname, 0.0)
    return total


def assert_optimal_match(
    packdb_rows: list[tuple],
    packdb_cols: list[str],
    oracle_result: SolverResult,
    decide_var_names: list[str],
    coeff_fn: Callable[[tuple], dict[str, float]],
    tolerance: float = 1e-4,
) -> None:
    """Assert that PackDB's objective matches the oracle's optimal value.

    This does NOT compare variable assignments — only objective values — because
    ILP problems often have multiple optimal solutions.

    Args:
        packdb_rows: Rows from PackDB query.
        packdb_cols: Column names from PackDB result description.
        oracle_result: Result from the oracle solver.
        decide_var_names: DECIDE variable column names in PackDB output.
        coeff_fn: Maps a PackDB row to objective coefficients per variable.
        tolerance: Acceptable absolute difference between objectives.
    """
    assert oracle_result.status == SolverStatus.OPTIMAL, (
        f"Oracle did not find optimal solution: {oracle_result.status}"
    )
    assert oracle_result.objective_value is not None

    packdb_obj = compute_packdb_objective(
        packdb_rows, packdb_cols, decide_var_names, coeff_fn
    )
    oracle_obj = oracle_result.objective_value

    diff = abs(packdb_obj - oracle_obj)
    assert diff <= tolerance, (
        f"Objective mismatch: PackDB={packdb_obj:.6f}, "
        f"Oracle={oracle_obj:.6f}, diff={diff:.6f} (tolerance={tolerance})"
    )


def assert_infeasible(packdb_conn, sql: str) -> None:
    """Assert that executing *sql* on packdb raises an infeasibility error."""
    import packdb as _packdb

    with pytest.raises(_packdb.InvalidInputException, match=r"(?i)infeasible"):
        packdb_conn.execute(sql)
