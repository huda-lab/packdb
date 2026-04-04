"""Tests for AVG() over decision variables.

AVG(expr) is rewritten to SUM(expr) with RHS scaling by row count N.
For objectives, AVG and SUM share the same argmax/argmin so no scaling needed.
For constraints, AVG(expr) op K becomes SUM(expr) op K*N where N depends on
WHEN/PER context.

Covers:
  - test_avg_objective: MAXIMIZE AVG(x * col) same result as MAXIMIZE SUM(x * col)
  - test_avg_constraint: AVG(x * weight) <= K
  - test_avg_with_when: N_when used (not total N)
  - test_avg_with_per: N_g used per group
  - test_avg_with_when_per: combined case
  - test_avg_boolean: AVG(x) for BOOLEAN vars
  - test_avg_integer: AVG(x) for INTEGER vars
  - test_avg_bilinear_constraint: AVG(x * y) bilinear constraint (Bool×Bool)
  - test_avg_no_decide_var: AVG(col) passes through to normal DuckDB
"""

import pytest


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_objective(packdb_cli):
    """MAXIMIZE AVG(x * col) should give same assignment as MAXIMIZE SUM(x * col)."""
    base_sql = """
        SELECT name, x FROM (
            VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)
        ) t(name, value)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        {objective}
    """
    rows_avg, cols_avg = packdb_cli.execute(base_sql.format(objective="MAXIMIZE AVG(x * value)"))
    rows_sum, cols_sum = packdb_cli.execute(base_sql.format(objective="MAXIMIZE SUM(x * value)"))

    ci_avg = {name: i for i, name in enumerate(cols_avg)}
    ci_sum = {name: i for i, name in enumerate(cols_sum)}

    # Same rows should be selected (same argmax)
    selected_avg = {r[ci_avg["name"]] for r in rows_avg if r[ci_avg["x"]] == 1}
    selected_sum = {r[ci_sum["name"]] for r in rows_sum if r[ci_sum["x"]] == 1}
    assert selected_avg == selected_sum, f"AVG and SUM objectives should select same rows: {selected_avg} vs {selected_sum}"


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_constraint(packdb_cli):
    """AVG(x * weight) <= K constrains the average, not the sum."""
    # 4 rows with weights [10, 5, 8, 3]. AVG(x * weight) <= 5 means SUM(x * weight) <= 20.
    # With x IS BOOLEAN, selecting a=10, c=8 gives SUM=18 <= 20 (AVG=4.5 <= 5). OK.
    # Selecting a=10, c=8, b=5 gives SUM=23 > 20 (AVG=5.75 > 5). Not OK.
    sql = """
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)
        ) t(name, value)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) <= 5
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    total = sum(r[ci["x"]] * r[ci["value"]] for r in rows)
    n = len(rows)
    avg = total / n
    assert avg <= 5.0 + 1e-6, f"AVG should be <= 5, got {avg}"


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_with_when(packdb_cli):
    """AVG(x * weight) <= K WHEN cond uses N_when (matching row count), not total N."""
    # 4 rows: 2 are 'high', 2 are 'low'.
    # AVG(x * value) <= 6 WHEN tier = 'high' means SUM among high rows <= 6 * 2 = 12.
    # High rows have values 10 and 8. Selecting both gives SUM=18 > 12. So at most one high row.
    sql = """
        SELECT name, value, tier, x FROM (
            VALUES ('a', 10, 'high'), ('b', 5, 'low'), ('c', 8, 'high'), ('d', 3, 'low')
        ) t(name, value, tier)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) <= 6 WHEN tier = 'high'
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    # Check: among 'high' tier rows, AVG(x*value) <= 6
    high_rows = [r for r in rows if r[ci["tier"]] == "high"]
    high_sum = sum(r[ci["x"]] * r[ci["value"]] for r in high_rows)
    high_avg = high_sum / len(high_rows)
    assert high_avg <= 6.0 + 1e-6, f"AVG among 'high' rows should be <= 6, got {high_avg}"


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_with_per(packdb_cli):
    """AVG(x * value) <= K PER group uses N_g per group."""
    # Group A has 2 rows (values 10, 8), group B has 2 rows (values 5, 3).
    # AVG(x * value) <= 4 PER grp means:
    #   Group A: SUM(x*value) <= 4*2 = 8. Can select c=8 but not a=10.
    #   Group B: SUM(x*value) <= 4*2 = 8. Can select both b=5, d=3 (sum=8).
    sql = """
        SELECT name, value, grp, x FROM (
            VALUES ('a', 10, 'A'), ('b', 5, 'B'), ('c', 8, 'A'), ('d', 3, 'B')
        ) t(name, value, grp)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) <= 4 PER grp
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    # Check per-group AVG constraint
    from collections import defaultdict
    groups = defaultdict(list)
    for r in rows:
        groups[r[ci["grp"]]].append(r)

    for grp_name, grp_rows in groups.items():
        grp_sum = sum(r[ci["x"]] * r[ci["value"]] for r in grp_rows)
        grp_avg = grp_sum / len(grp_rows)
        assert grp_avg <= 4.0 + 1e-6, f"AVG for group '{grp_name}' should be <= 4, got {grp_avg}"


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_with_when_per(packdb_cli):
    """AVG with both WHEN and PER: filters then groups."""
    sql = """
        SELECT name, value, grp, tier, x FROM (
            VALUES ('a', 10, 'A', 'high'), ('b', 5, 'B', 'high'),
                   ('c', 8, 'A', 'low'),  ('d', 3, 'B', 'low'),
                   ('e', 6, 'A', 'high'), ('f', 4, 'B', 'high')
        ) t(name, value, grp, tier)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) <= 5 WHEN tier = 'high' PER grp
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    # Check: for each group, among 'high' tier rows, AVG(x*value) <= 5
    from collections import defaultdict
    groups = defaultdict(list)
    for r in rows:
        if r[ci["tier"]] == "high":
            groups[r[ci["grp"]]].append(r)

    for grp_name, grp_rows in groups.items():
        grp_sum = sum(r[ci["x"]] * r[ci["value"]] for r in grp_rows)
        grp_avg = grp_sum / len(grp_rows)
        assert grp_avg <= 5.0 + 1e-6, f"AVG for group '{grp_name}' (high tier) should be <= 5, got {grp_avg}"


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_boolean(packdb_cli):
    """AVG(x) for BOOLEAN variables = fraction of selected rows."""
    # AVG(x) <= 0.5 means SUM(x) <= 0.5 * 4 = 2
    sql = """
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5), ('c', 8), ('d', 3)
        ) t(name, value)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x) <= 0.5
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    selected = sum(r[ci["x"]] for r in rows)
    assert selected <= 2, f"AVG(x) <= 0.5 with 4 rows means at most 2 selected, got {selected}"
    # Should pick top 2 by value: a=10, c=8
    assert selected == 2, f"Should select exactly 2 to maximize, got {selected}"


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_integer(packdb_cli):
    """AVG(x) for INTEGER variables."""
    # 3 rows. AVG(x) <= 3 means SUM(x) <= 9.
    sql = """
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5), ('c', 8)
        ) t(name, value)
        DECIDE x
        SUCH THAT x <= 5 AND AVG(x) <= 3
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    total_x = sum(r[ci["x"]] for r in rows)
    n = len(rows)
    avg_x = total_x / n
    assert avg_x <= 3.0 + 1e-6, f"AVG(x) should be <= 3, got {avg_x}"


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_bilinear_constraint(packdb_cli):
    """AVG(x * y) where both x and y are BOOLEAN decision variables — bilinear constraint.

    With bilinear support, Bool×Bool products are linearized via AND-linearization.
    AVG(x*y) <= 0.5 with 3 rows means SUM(x*y) <= 1.5, i.e., at most 1 row has both x=1 and y=1.
    """
    sql = """
        SELECT x, y FROM (
            VALUES (1), (2), (3)
        ) t(value)
        DECIDE x IS BOOLEAN, y IS BOOLEAN
        SUCH THAT AVG(x * y) <= 0.5
        MAXIMIZE SUM(x + y)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    # Verify constraint: AVG(x*y) <= 0.5 → at most 1 row with both x=1 and y=1
    both_selected = sum(1 for r in rows if r[ci["x"]] == 1 and r[ci["y"]] == 1)
    n = len(rows)
    avg_xy = both_selected / n
    assert avg_xy <= 0.5 + 1e-6, f"AVG(x*y) should be <= 0.5, got {avg_xy}"
    # Objective MAXIMIZE SUM(x+y): should maximize total selections
    total = sum(r[ci["x"]] + r[ci["y"]] for r in rows)
    assert total >= 4, f"Should select at least 4 total (x+y across rows), got {total}"


@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_avg_no_decide_var(packdb_cli):
    """AVG(col) without decide variables should pass through to normal DuckDB."""
    sql = """
        SELECT AVG(value) as avg_val FROM (
            VALUES (10), (5), (8), (3)
        ) t(value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}
    assert abs(rows[0][ci["avg_val"]] - 6.5) < 1e-6, f"Expected AVG=6.5, got {rows[0][ci['avg_val']]}"
