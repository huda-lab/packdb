"""Tests for the PER keyword (not yet implemented).

PER partitions constraints by group:
    SUM(x) <= 5 PER s_nationkey
means a separate SUM(x) <= 5 constraint for each distinct s_nationkey value.
"""

import pytest


@pytest.mark.per_clause
@pytest.mark.xfail(reason="PER keyword not yet implemented", strict=True)
def test_per_basic(packdb_cli):
    """PER keyword should partition constraints by group."""
    packdb_cli.execute("""
        SELECT s_suppkey, s_acctbal, x FROM supplier
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """)


@pytest.mark.per_clause
@pytest.mark.xfail(reason="PER keyword not yet implemented", strict=True)
def test_per_with_integer_variable(packdb_cli):
    """PER with integer variables and a weighted constraint."""
    packdb_cli.execute("""
        SELECT ps_partkey, ps_availqty, ps_supplycost, x
        FROM partsupp WHERE ps_partkey < 50
        DECIDE x IS INTEGER
        SUCH THAT SUM(x * ps_supplycost) <= 1000 PER ps_partkey
        MAXIMIZE SUM(x * ps_availqty)
    """)


@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.xfail(reason="PER keyword not yet implemented", strict=True)
def test_per_combined_with_when(packdb_cli):
    """PER and WHEN used together — group-level constraint with row filter."""
    packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity,
               l_returnflag, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 50 PER l_returnflag
            AND SUM(x) <= 30
        MAXIMIZE SUM(x * l_extendedprice) WHEN l_returnflag = 'R'
    """)
