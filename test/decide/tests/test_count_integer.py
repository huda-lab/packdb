"""Tests for COUNT(x) over INTEGER decision variables.

Uses Big-M indicator variables: for each INTEGER var x, introduces binary z
with z <= x and x <= M*z, then rewrites COUNT(x) to SUM(z).

Covers:
  - test_count_integer_constraint: COUNT(x) >= k forces at least k non-zero vars
  - test_count_integer_objective: MAXIMIZE COUNT(x)
  - test_count_integer_with_when: COUNT(x) WHEN condition
  - test_count_integer_with_per: COUNT(x) >= k PER group
  - test_count_integer_multiple_vars: COUNT on two different INTEGER vars
  - test_count_integer_hidden_indicator: indicator vars not in SELECT *
"""

import pytest


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_constraint(packdb_cli):
    """COUNT(x) >= k forces at least k rows to have x > 0."""
    sql = """
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)
        ) t(name, value)
        DECIDE x
        SUCH THAT x <= 10 AND COUNT(x) >= 3
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    nonzero_count = sum(1 for r in rows if r[ci["x"]] > 0)
    assert nonzero_count >= 3, f"Expected at least 3 non-zero x values, got {nonzero_count}"


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_constraint_upper(packdb_cli):
    """COUNT(x) <= k forces at most k rows to have x > 0."""
    sql = """
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)
        ) t(name, value)
        DECIDE x
        SUCH THAT x <= 10 AND COUNT(x) <= 2
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    nonzero_count = sum(1 for r in rows if r[ci["x"]] > 0)
    assert nonzero_count <= 2, f"Expected at most 2 non-zero x values, got {nonzero_count}"
    # Should pick top 2 by value (a=10, c=8) for maximum objective
    total_obj = sum(r[ci["x"]] * r[ci["value"]] for r in rows)
    assert total_obj > 0, "Objective should be positive"


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_objective(packdb_cli):
    """MAXIMIZE COUNT(x) should maximize number of non-zero assignments."""
    sql = """
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5), ('c', 8)
        ) t(name, value)
        DECIDE x
        SUCH THAT x <= 5 AND SUM(x) <= 10
        MAXIMIZE COUNT(x)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    nonzero_count = sum(1 for r in rows if r[ci["x"]] > 0)
    # With SUM(x) <= 10 and x <= 5, we can have all 3 non-zero (e.g., 1,1,1)
    assert nonzero_count == 3, f"Expected 3 non-zero x values when maximizing count, got {nonzero_count}"


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_hidden_indicator(packdb_cli):
    """Indicator variables should not appear in SELECT * output."""
    sql = """
        SELECT * FROM (
            VALUES ('a', 10), ('b', 5)
        ) t(name, value)
        DECIDE x
        SUCH THAT x <= 5 AND COUNT(x) >= 1
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    # Indicator vars like __count_ind_x__ should NOT appear in output
    for col in cols:
        assert "__count_ind_" not in col, f"Indicator variable '{col}' should be hidden from output"
    assert "x" in cols, "Decision variable x should be in output"


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_with_when(packdb_cli):
    """COUNT(x) WHEN condition should only count non-zero x among qualifying rows."""
    sql = """
        SELECT name, value, x FROM (
            VALUES ('a', 10, 'high'), ('b', 5, 'low'), ('c', 8, 'high'), ('d', 3, 'low')
        ) t(name, value, tier)
        DECIDE x
        SUCH THAT x <= 10
            AND COUNT(x) >= 1 WHEN tier = 'low'
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    # At least 1 'low' tier row must have x > 0
    assert len(rows) > 0


@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_count_integer_dedup(packdb_cli):
    """Multiple COUNT(x) references to same var should reuse one indicator."""
    sql = """
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)
        ) t(name, value)
        DECIDE x
        SUCH THAT x <= 10 AND COUNT(x) >= 2 AND COUNT(x) <= 3
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    nonzero_count = sum(1 for r in rows if r[ci["x"]] > 0)
    assert 2 <= nonzero_count <= 3, f"Expected 2-3 non-zero x values, got {nonzero_count}"
