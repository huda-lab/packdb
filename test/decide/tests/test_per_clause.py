"""Tests for the PER keyword (not yet implemented).

PER allows per-group constraints:
    SUM(x) <= 5 PER s_nationkey
"""

import pytest


@pytest.mark.per_clause
@pytest.mark.xfail(reason="PER keyword not yet implemented", strict=True)
def test_per_basic(packdb_conn):
    """PER keyword should partition constraints by group."""
    packdb_conn.execute("""
        SELECT s_suppkey, s_acctbal, x FROM supplier
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """)
