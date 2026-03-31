"""Tests for the PER keyword.

PER partitions constraints by group:
    SUM(x) <= 5 PER s_nationkey
means a separate SUM(x) <= 5 constraint for each distinct s_nationkey value.

Also covers:
  - PER with <> operator (Big-M disjunction per group)
  - NULL values in PER column (excluded from all groups)
  - Two PER constraints on different grouping columns
"""

import pytest


@pytest.mark.per_clause
def test_per_basic(packdb_cli):
    """PER keyword should partition constraints by group."""
    packdb_cli.execute("""
        SELECT s_suppkey, s_acctbal, x FROM supplier
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5 PER s_nationkey
        MAXIMIZE SUM(x * s_acctbal)
    """)


@pytest.mark.per_clause
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


@pytest.mark.per_clause
@pytest.mark.cons_comparison
def test_per_not_equal(packdb_cli):
    """PER with <> operator — Big-M disjunction generated per group."""
    result, cols = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_returnflag, x
        FROM lineitem WHERE l_orderkey < 50
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <> 3 PER l_returnflag
            AND SUM(x) <= 15
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0

    flag_idx = cols.index("l_returnflag")
    x_idx = cols.index("x")

    # Verify per-group <> constraint
    group_sums = {}
    for row in result:
        flag = str(row[flag_idx])
        group_sums[flag] = group_sums.get(flag, 0) + int(row[x_idx])

    for flag, total in group_sums.items():
        assert total != 3, \
            f"PER <> constraint violated: SUM(x) = {total} for flag='{flag}' (expected <> 3)"


@pytest.mark.per_clause
@pytest.mark.edge_case
def test_per_null_group_key(packdb_cli):
    """NULL values in PER column should be excluded from all groups."""
    result, cols = packdb_cli.execute("""
        WITH data AS (
            SELECT 1 AS id, 'A' AS grp, 10.0 AS val UNION ALL
            SELECT 2, NULL, 50.0 UNION ALL
            SELECT 3, 'B', 8.0 UNION ALL
            SELECT 4, NULL, 30.0 UNION ALL
            SELECT 5, 'A', 12.0
        )
        SELECT id, grp, val, x
        FROM data
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 1 PER grp
        MAXIMIZE SUM(x * val)
    """)
    assert len(result) > 0

    grp_idx = cols.index("grp")
    x_idx = cols.index("x")

    # Verify per-group constraint for non-NULL groups
    group_sums = {}
    for row in result:
        grp = row[grp_idx]
        if grp is not None:
            group_sums[grp] = group_sums.get(grp, 0) + int(row[x_idx])

    for grp, total in group_sums.items():
        assert total <= 1, \
            f"PER constraint violated: SUM(x) = {total} > 1 for group '{grp}'"


@pytest.mark.per_clause
def test_per_different_grouping_columns(packdb_cli):
    """Two PER constraints on different columns — overlapping group structures."""
    result, cols = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_returnflag, l_linestatus,
               l_extendedprice, x
        FROM lineitem WHERE l_orderkey < 100
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 5 PER l_returnflag
            AND SUM(x) <= 8 PER l_linestatus
        MAXIMIZE SUM(x * l_extendedprice)
    """)
    assert len(result) > 0

    flag_idx = cols.index("l_returnflag")
    status_idx = cols.index("l_linestatus")
    x_idx = cols.index("x")

    # Verify both PER constraints
    flag_sums = {}
    status_sums = {}
    for row in result:
        x_val = int(row[x_idx])
        flag = str(row[flag_idx])
        status = str(row[status_idx])
        flag_sums[flag] = flag_sums.get(flag, 0) + x_val
        status_sums[status] = status_sums.get(status, 0) + x_val

    for flag, total in flag_sums.items():
        assert total <= 5, \
            f"PER l_returnflag violated: SUM(x) = {total} > 5 for flag='{flag}'"
    for status, total in status_sums.items():
        assert total <= 8, \
            f"PER l_linestatus violated: SUM(x) = {total} > 8 for status='{status}'"
