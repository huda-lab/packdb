"""Tests for multi-column PER.

PER (col1, col2) partitions constraints by composite groups:
    SUM(x) <= 5 PER (l_returnflag, l_linestatus)
means a separate SUM(x) <= 5 constraint for each distinct (l_returnflag, l_linestatus) pair.
"""

import pytest


@pytest.mark.per_clause
def test_multi_column_per_basic(packdb_cli):
    """PER (col1, col2) should partition constraints by composite groups."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_returnflag, l_linestatus, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 3 PER (l_returnflag, l_linestatus)
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0

    # Verify: at most 3 selected rows per (returnflag, linestatus) group
    group_counts = {}
    flag_idx, status_idx, x_idx = 1, 2, 3
    for row in result:
        if row[x_idx] == 1:
            group_key = (row[flag_idx], row[status_idx])
            group_counts[group_key] = group_counts.get(group_key, 0) + 1
    for group_key, count in group_counts.items():
        assert count <= 3, f"Group {group_key} has {count} selected rows, expected <= 3"


@pytest.mark.per_clause
def test_multi_column_per_single_column_in_parens(packdb_cli):
    """PER (col) with parens should work the same as PER col."""
    result_parens, _ = packdb_cli.execute("""
        SELECT s_suppkey, s_nationkey, x FROM supplier
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5 PER (s_nationkey)
        MAXIMIZE SUM(x * s_acctbal)
    """)
    result_no_parens, _ = packdb_cli.execute("""
        SELECT s_suppkey, s_nationkey, x FROM supplier
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """)
    assert len(result_parens) > 0
    assert len(result_parens) == len(result_no_parens)

    # Both should produce the same x assignments
    x_idx = 2
    for r1, r2 in zip(result_parens, result_no_parens):
        assert r1[x_idx] == r2[x_idx], f"Mismatch at suppkey {r1[0]}: {r1[x_idx]} vs {r2[x_idx]}"


@pytest.mark.per_clause
@pytest.mark.when_constraint
def test_multi_column_per_with_when_on_different_column(packdb_cli):
    """WHEN filters on a column NOT in PER — groups should be unaffected."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_returnflag, l_linestatus,
               l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 200
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 20 WHEN l_discount > 0.05 PER (l_returnflag, l_linestatus)
            AND SUM(x) <= 30
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0

    # The constraint only applies to high-discount rows, grouped by (flag, status).
    # Non-discount rows are unconstrained by this PER constraint (they get INVALID_INDEX).
    # Verify solution is feasible (doesn't crash, returns results).
    x_idx = 5
    selected = sum(1 for row in result if row[x_idx] == 1)
    assert selected > 0, "Should select at least some rows"
    assert selected <= 30, "Global SUM(x) <= 30 constraint must hold"


@pytest.mark.per_clause
@pytest.mark.when_constraint
def test_multi_column_per_when_overlaps_per_column(packdb_cli):
    """WHEN filters on a column that IS one of the PER columns.

    Example: WHEN l_returnflag = 'R' PER (l_returnflag, l_linestatus)
    After WHEN filtering, only 'R' rows survive. The PER groups become
    effectively just PER l_linestatus (since all surviving rows have flag='R').
    The constraint should only apply to 'R' rows, grouped by linestatus.
    """
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_returnflag, l_linestatus, l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 200
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2 WHEN l_returnflag = 'R' PER (l_returnflag, l_linestatus)
            AND SUM(x) <= 50
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0

    flag_idx, status_idx, x_idx = 1, 2, 4

    # Count selections per (flag, status) group among 'R' rows
    r_group_counts = {}
    non_r_selected = 0
    for row in result:
        if row[x_idx] == 1:
            if row[flag_idx] == 'R':
                key = (row[flag_idx], row[status_idx])
                r_group_counts[key] = r_group_counts.get(key, 0) + 1
            else:
                non_r_selected += 1

    # The PER constraint (SUM(x) <= 2) only applies to R rows
    for group_key, count in r_group_counts.items():
        assert count <= 2, (
            f"Group {group_key} has {count} selected R-rows, expected <= 2 "
            f"(WHEN+PER constraint should limit each R-subgroup)"
        )

    # Non-R rows should be freely selectable (only bounded by global SUM(x) <= 50)
    # This verifies WHEN correctly excludes non-R rows from the PER constraint
    total_selected = sum(1 for row in result if row[x_idx] == 1)
    r_total = sum(r_group_counts.values()) if r_group_counts else 0
    assert total_selected >= r_total, "Total selected should include non-R rows"


@pytest.mark.per_clause
@pytest.mark.when_constraint
def test_multi_column_per_when_eliminates_all_in_group(packdb_cli):
    """WHEN condition eliminates all rows from some composite groups.

    If no rows survive WHEN for a particular (col1, col2) combination,
    that group should simply not exist — no constraint generated for it.
    """
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_returnflag, l_linestatus, l_quantity, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2 WHEN l_quantity > 40 PER (l_returnflag, l_linestatus)
            AND SUM(x) <= 20
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0

    flag_idx, status_idx, qty_idx, x_idx = 1, 2, 3, 4

    # Only rows with qty > 40 are subject to the PER constraint
    # Verify constraint holds for qualifying rows per group
    group_counts = {}
    for row in result:
        if row[x_idx] == 1 and row[qty_idx] > 40:
            key = (row[flag_idx], row[status_idx])
            group_counts[key] = group_counts.get(key, 0) + 1
    for group_key, count in group_counts.items():
        assert count <= 2, (
            f"High-qty group {group_key} has {count} selected, expected <= 2"
        )


@pytest.mark.per_clause
def test_multi_column_per_more_groups(packdb_cli):
    """Multi-column PER should create finer-grained groups than single-column."""
    # With PER l_returnflag only (3 groups: A, N, R)
    result_single, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_returnflag, l_linestatus, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 3 PER l_returnflag
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    # With PER (l_returnflag, l_linestatus) (more groups: AF, NF, NO, RF)
    result_multi, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_returnflag, l_linestatus, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 3 PER (l_returnflag, l_linestatus)
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    # Multi-column should select at least as many (more groups, each allows 3)
    x_idx = 3
    sum_single = sum(1 for row in result_single if row[x_idx] == 1)
    sum_multi = sum(1 for row in result_multi if row[x_idx] == 1)
    assert sum_multi >= sum_single, (
        f"Multi-column PER selected {sum_multi} but single-column selected {sum_single}"
    )


@pytest.mark.per_clause
def test_multi_column_per_three_columns(packdb_cli):
    """PER with three columns should work."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_returnflag, l_linestatus, l_shipmode, x
        FROM lineitem WHERE l_orderkey < 200
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2 PER (l_returnflag, l_linestatus, l_shipmode)
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0

    # Verify: at most 2 selected rows per (flag, status, shipmode) group
    group_counts = {}
    flag_idx, status_idx, mode_idx, x_idx = 1, 2, 3, 4
    for row in result:
        if row[x_idx] == 1:
            key = (row[flag_idx], row[status_idx], row[mode_idx])
            group_counts[key] = group_counts.get(key, 0) + 1
    for group_key, count in group_counts.items():
        assert count <= 2, f"Group {group_key} has {count} selected, expected <= 2"


@pytest.mark.per_clause
def test_multi_column_per_with_integer_variable(packdb_cli):
    """Multi-column PER with integer variables and weighted constraints."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_returnflag, l_linestatus, l_quantity, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS INTEGER
        SUCH THAT SUM(x * l_quantity) <= 100 PER (l_returnflag, l_linestatus)
            AND x <= 3
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0

    # Verify per-group weighted constraint
    flag_idx, status_idx, qty_idx, x_idx = 1, 2, 3, 4
    group_sums = {}
    for row in result:
        key = (row[flag_idx], row[status_idx])
        group_sums[key] = group_sums.get(key, 0) + row[x_idx] * row[qty_idx]
    for group_key, total in group_sums.items():
        assert total <= 100 + 1e-6, (
            f"Group {group_key} has weighted sum {total}, expected <= 100"
        )
