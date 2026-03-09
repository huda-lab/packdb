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
def test_multi_column_per_with_when(packdb_cli):
    """PER (col1, col2) combined with WHEN should filter then group."""
    result, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
               l_extendedprice, l_quantity, x
        FROM lineitem WHERE l_orderkey < 200
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * l_quantity) <= 20 WHEN l_returnflag = 'R' PER (l_returnflag, l_linestatus)
            AND SUM(x) <= 30
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0


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
