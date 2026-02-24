"""Infeasibility error tests.

Tests that PackDB correctly detects and reports infeasible models.
"""

import pytest

import packdb


@pytest.mark.error_infeasible
@pytest.mark.error
class TestInfeasibleModels:
    """PackDB should raise InvalidInputException for infeasible problems."""

    def test_contradictory_per_row_bounds(self, packdb_conn):
        """Per-row bounds x >= 10 AND x <= 5 are contradictory."""
        with pytest.raises(packdb.InvalidInputException, match=r"(?i)infeasible"):
            packdb_conn.execute("""
                SELECT l_quantity, x FROM lineitem WHERE l_orderkey < 10
                DECIDE x IS INTEGER
                SUCH THAT x >= 10 AND x <= 5
                MAXIMIZE SUM(x * l_quantity)
            """)

    def test_impossible_sum_constraint(self, packdb_conn):
        """Boolean variables can only sum to at most N rows; require more."""
        with pytest.raises(packdb.InvalidInputException, match=r"(?i)infeasible"):
            packdb_conn.execute("""
                SELECT l_quantity, x FROM lineitem WHERE l_orderkey = 1
                DECIDE x IS BOOLEAN
                SUCH THAT SUM(x) >= 999999
                MAXIMIZE SUM(x * l_quantity)
            """)

    def test_negative_sum_upper_bound(self, packdb_conn):
        """Non-negative variables cannot have a negative SUM."""
        with pytest.raises(packdb.InvalidInputException, match=r"(?i)infeasible"):
            packdb_conn.execute("""
                SELECT l_quantity, x FROM lineitem WHERE l_orderkey < 5
                DECIDE x IS BOOLEAN
                SUCH THAT SUM(x) >= 1 AND SUM(x * l_quantity) <= -1
                MAXIMIZE SUM(x)
            """)
