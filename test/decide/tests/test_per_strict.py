"""Tests for PER STRICT keyword.

PER STRICT switches from WHEN→PER (default) to PER→WHEN evaluation order.
The only difference: empty groups (where all rows fail WHEN) still emit
constraints instead of being skipped.

AGG(∅) evaluates per standard math: SUM(∅)=0, MAX(∅)=-∞, MIN(∅)=+∞.
"""

import pytest


@pytest.mark.per_strict
@pytest.mark.per_clause
class TestPerStrictConstraints:

    def test_sum_ge_empty_group_infeasible(self, packdb_cli):
        """SUM >= with empty group: 0 >= 1 is infeasible.

        l_returnflag has values 'A', 'N', 'R'. WHEN filters to 'R' only.
        PER STRICT forces constraints on all groups including A and N (empty).
        SUM(∅) = 0 >= 1 for empty groups → infeasible.
        """
        packdb_cli.assert_error("""
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) >= 1 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """, match=r"(?i)(infeasible|unbounded)")

    def test_sum_le_empty_group_trivially_true(self, packdb_cli):
        """SUM <= with empty group: 0 <= 100 is trivially true.

        Same setup but upper bound — empty groups pass the constraint.
        Should produce a feasible solution identical to non-STRICT.
        """
        result, cols = packdb_cli.execute("""
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) <= 100 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """)
        assert len(result) > 0

    def test_sum_eq_empty_group_infeasible(self, packdb_cli):
        """SUM = with empty group: 0 = 3 is infeasible."""
        packdb_cli.assert_error("""
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) = 3 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """, match=r"(?i)(infeasible|unbounded)")

    def test_per_strict_without_when_same_as_per(self, packdb_cli):
        """PER STRICT without WHEN — no empty groups possible, same as PER.

        When there's no WHEN filter, every group has rows, so STRICT changes nothing.
        """
        result_strict, cols_strict = packdb_cli.execute("""
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 30
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) <= 5 PER STRICT l_returnflag
            MAXIMIZE SUM(x * l_extendedprice)
        """)

        result_normal, cols_normal = packdb_cli.execute("""
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 30
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) <= 5 PER l_returnflag
            MAXIMIZE SUM(x * l_extendedprice)
        """)

        # Both should produce same number of rows
        assert len(result_strict) == len(result_normal)

    def test_convergent_case_feasible(self, packdb_cli):
        """Empty group is trivially satisfied — solver should find solution.

        SUM(x) <= 50 with WHEN filtering: empty groups get 0 <= 50 (true).
        """
        result, cols = packdb_cli.execute("""
            SELECT l_orderkey, l_returnflag, l_quantity, x
            FROM lineitem WHERE l_orderkey < 50
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x * l_quantity) <= 50
                WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x * l_extendedprice)
        """)
        assert len(result) > 0

    def test_multi_column_per_strict(self, packdb_cli):
        """PER STRICT with multi-column grouping."""
        result, cols = packdb_cli.execute("""
            SELECT l_orderkey, l_returnflag, l_linestatus, x
            FROM lineitem WHERE l_orderkey < 30
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) <= 3 PER STRICT (l_returnflag, l_linestatus)
            MAXIMIZE SUM(x * l_extendedprice)
        """)
        assert len(result) > 0

        # Verify per-group constraints
        flag_idx = cols.index("l_returnflag")
        status_idx = cols.index("l_linestatus")
        x_idx = cols.index("x")

        group_sums = {}
        for row in result:
            key = (str(row[flag_idx]), str(row[status_idx]))
            group_sums[key] = group_sums.get(key, 0) + int(row[x_idx])

        for key, total in group_sums.items():
            assert total <= 3, \
                f"PER STRICT constraint violated: SUM(x) = {total} for group {key}"


@pytest.mark.per_strict
@pytest.mark.per_clause
@pytest.mark.min_max
class TestPerStrictMinMax:

    def test_max_ge_empty_group_infeasible(self, packdb_cli):
        """MAX >= with empty group (hard case): infeasible.

        MAX(∅) = -∞, so -∞ >= 1 is false.
        The hard formulation produces SUM(y) >= 1 with 0 indicators → 0 >= 1.
        """
        packdb_cli.assert_error("""
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS INTEGER
            SUCH THAT x <= 5
                AND MAX(x) >= 1 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """, match=r"(?i)(infeasible|unbounded)")

    def test_min_le_empty_group_infeasible(self, packdb_cli):
        """MIN <= with empty group (hard case): infeasible.

        MIN(∅) = +∞, so +∞ <= 3 is false.
        The hard formulation produces SUM(y) >= 1 with 0 indicators → 0 >= 1.
        """
        packdb_cli.assert_error("""
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS INTEGER
            SUCH THAT x <= 5
                AND MIN(x) <= 3 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """, match=r"(?i)(infeasible|unbounded)")

    def test_max_le_empty_group_vacuously_true(self, packdb_cli):
        """MAX <= with empty group (easy case): vacuously true.

        MAX(∅) = -∞, so -∞ <= 12 is true. Nobody to violate the cap.
        The easy formulation produces per-row constraints; empty groups
        produce zero constraints, which is vacuously satisfied.
        """
        result, cols = packdb_cli.execute("""
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS INTEGER
            SUCH THAT x <= 5
                AND MAX(x) <= 12 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """)
        assert len(result) > 0

    def test_min_ge_empty_group_vacuously_true(self, packdb_cli):
        """MIN >= with empty group (easy case): vacuously true.

        MIN(∅) = +∞, so +∞ >= 1 is true. Nobody below the floor.
        """
        result, cols = packdb_cli.execute("""
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 10
            DECIDE x IS INTEGER
            SUCH THAT x <= 5
                AND MIN(x) >= 1 WHEN l_returnflag = 'R' PER STRICT l_returnflag
            MAXIMIZE SUM(x)
        """)
        assert len(result) > 0


@pytest.mark.per_strict
@pytest.mark.per_clause
@pytest.mark.cons_comparison
class TestPerStrictNE:

    def test_ne_empty_group_trivially_true(self, packdb_cli):
        """SUM <> K with empty group, K != 0: trivially true.

        SUM(∅) = 0, 0 <> 3 is true. No infeasibility.
        """
        result, cols = packdb_cli.execute("""
            SELECT l_orderkey, l_returnflag, x
            FROM lineitem WHERE l_orderkey < 30
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) <> 3 WHEN l_returnflag = 'R' PER STRICT l_returnflag
                AND SUM(x) <= 15
            MAXIMIZE SUM(x * l_extendedprice)
        """)
        assert len(result) > 0
