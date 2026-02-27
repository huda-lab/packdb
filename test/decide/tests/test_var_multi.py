"""Tests for multiple decision variables (DECIDE x, y, ...).

Multiple DECIDE variables allow modeling richer problems where rows have
more than one decision per tuple.  The binder accepts this syntax;
whether the solver fully handles it is unverified.
"""

import pytest


@pytest.mark.var_multi
@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.xfail(
    reason="Multiple DECIDE variables may not be fully supported",
    strict=False,
)
def test_two_variables_separate_constraints(packdb_conn):
    """DECIDE x IS BOOLEAN, y IS INTEGER with independent constraints."""
    result = packdb_conn.execute("""
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x, y
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN, y IS INTEGER
        SUCH THAT SUM(x * l_quantity) <= 100
            AND y <= 5
            AND SUM(y) <= 20
        MAXIMIZE SUM(x * l_extendedprice)
    """).fetchall()
    assert len(result) > 0


@pytest.mark.var_multi
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.xfail(
    reason="Multiple DECIDE variables may not be fully supported",
    strict=False,
)
def test_two_boolean_variables(packdb_conn):
    """Two boolean variables with a cross-constraint."""
    result = packdb_conn.execute("""
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x, y
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN, y IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 100
            AND SUM(y * l_quantity) <= 200
        MAXIMIZE SUM(x * l_extendedprice + y * l_quantity)
    """).fetchall()
    assert len(result) > 0
