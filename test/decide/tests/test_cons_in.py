"""Tests for IN (...) constraints on decision variables.

x IN (0, 1, 3) restricts the variable's domain to a discrete set.
The binder accepts this syntax; whether the solver handles it is unverified.
"""

import pytest


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_maximize
@pytest.mark.xfail(
    reason="IN domain constraints may not be fully supported by the solver",
    strict=False,
)
def test_in_domain_restriction(packdb_cli):
    """x IN (0, 1, 3) — restrict integer variable to a sparse domain."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (0, 1, 3)
            AND SUM(x * l_quantity) <= 200
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    x_idx = 4
    for row in result:
        assert row[x_idx] in (0, 1, 3), f"x={row[x_idx]} not in allowed domain"


@pytest.mark.cons_in
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
@pytest.mark.xfail(
    reason="IN domain constraints may not be fully supported by the solver",
    strict=False,
)
def test_in_binary_domain(packdb_cli):
    """x IN (0, 1) on an implicitly typed variable — equivalent to IS BOOLEAN."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (0, 1)
            AND SUM(x * l_quantity) <= 100
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    x_idx = 4
    for row in result:
        assert row[x_idx] in (0, 1), f"x={row[x_idx]} not in {{0, 1}}"
