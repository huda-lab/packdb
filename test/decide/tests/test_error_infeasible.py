"""Infeasibility error tests.

Tests that PackDB correctly detects and reports infeasible models.
"""

import pytest


@pytest.mark.error_infeasible
@pytest.mark.error
class TestInfeasibleModels:
    """PackDB should raise InvalidInputException for infeasible problems."""

    def test_contradictory_per_row_bounds(self, packdb_cli):
        """Per-row bounds x >= 10 AND x <= 5 are contradictory."""
        packdb_cli.assert_error("""
                SELECT l_quantity, x FROM lineitem WHERE l_orderkey < 10
                DECIDE x IS INTEGER
                SUCH THAT x >= 10 AND x <= 5
                MAXIMIZE SUM(x * l_quantity)
            """, match=r"(?i)(infeasible|unbounded)")

    def test_impossible_sum_constraint(self, packdb_cli):
        """Boolean variables can only sum to at most N rows; require more."""
        packdb_cli.assert_error("""
                SELECT l_quantity, x FROM lineitem WHERE l_orderkey = 1
                DECIDE x IS BOOLEAN
                SUCH THAT SUM(x) >= 999999
                MAXIMIZE SUM(x * l_quantity)
            """, match=r"(?i)(infeasible|unbounded)")

    def test_negative_sum_upper_bound(self, packdb_cli):
        """Non-negative variables cannot have a negative SUM."""
        packdb_cli.assert_error("""
                SELECT l_quantity, x FROM lineitem WHERE l_orderkey < 5
                DECIDE x IS BOOLEAN
                SUCH THAT SUM(x) >= 1 AND SUM(x * l_quantity) <= -1
                MAXIMIZE SUM(x)
            """, match=r"(?i)(infeasible|unbounded)")

    def test_infeasible_when_forces_all_zero(self, packdb_cli):
        """WHEN forces all x=0 while aggregate requires SUM(x) >= 1."""
        packdb_cli.assert_error("""
                SELECT l_orderkey, l_quantity, l_returnflag, x
                FROM lineitem WHERE l_orderkey < 10
                DECIDE x IS BOOLEAN
                SUCH THAT x <= 0 WHEN l_quantity > 0
                    AND SUM(x) >= 1
                MAXIMIZE SUM(x * l_quantity)
            """, match=r"(?i)(infeasible|unbounded|WHEN conditions)")
