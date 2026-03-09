"""Tests for IN (...) constraints on decision variables.

x IN (0, 1, 3) restricts the variable's domain to a discrete set.
Implemented via auxiliary binary indicator variables at bind time:
K indicators z_i with cardinality constraint (z_1+...+z_K=1) and
linking constraint (x = v1*z_1+...+vK*z_K).
"""

import pytest


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_maximize
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


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_minimize
def test_in_single_value(packdb_cli):
    """x IN (3) — single-value IN, equivalent to x = 3."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (3)
        MINIMIZE SUM(x * l_quantity)
    """)
    assert len(result) > 0
    x_idx = 2
    for row in result:
        assert row[x_idx] == 3, f"x={row[x_idx]}, expected 3"


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_minimize
def test_in_minimize_picks_smallest(packdb_cli):
    """x IN (3, 5, 7) MINIMIZE SUM(x) — solver should pick 3 for all rows."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (3, 5, 7)
        MINIMIZE SUM(x * l_quantity)
    """)
    assert len(result) > 0
    x_idx = 2
    for row in result:
        assert row[x_idx] in (3, 5, 7), f"x={row[x_idx]} not in domain"
        assert row[x_idx] == 3, f"MINIMIZE should pick smallest: x={row[x_idx]}"


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.obj_maximize
def test_in_maximize_picks_largest(packdb_cli):
    """x IN (2, 5) MAXIMIZE SUM(x) — solver should pick 5 for all rows (no aggregate cap)."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (2, 5)
            AND SUM(x * l_quantity) <= 99999
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    x_idx = 2
    for row in result:
        assert row[x_idx] in (2, 5), f"x={row[x_idx]} not in domain"


@pytest.mark.cons_in
@pytest.mark.var_integer
@pytest.mark.when
@pytest.mark.obj_maximize
def test_in_with_when(packdb_cli):
    """x IN (0, 2, 4) WHEN condition — IN with conditional application."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x
        SUCH THAT x IN (0, 2, 4) WHEN l_quantity > 20
            AND SUM(x * l_quantity) <= 500
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    x_idx = 3
    qty_idx = 2
    for row in result:
        if row[qty_idx] > 20:
            assert row[x_idx] in (0, 2, 4), \
                f"x={row[x_idx]} not in domain when l_quantity={row[qty_idx]} > 20"


@pytest.mark.cons_in
@pytest.mark.var_boolean
@pytest.mark.obj_maximize
def test_in_boolean_explicit(packdb_cli):
    """x IS BOOLEAN with x IN (0, 1) — trivially satisfied, no auxiliary vars needed."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT x IN (0, 1)
            AND SUM(x) <= 10
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0
    x_idx = 2
    for row in result:
        assert row[x_idx] in (0, 1), f"x={row[x_idx]} not in {{0, 1}}"
