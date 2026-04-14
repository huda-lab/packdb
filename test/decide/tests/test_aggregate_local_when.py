"""Tests for aggregate-local WHEN filters inside DECIDE aggregate expressions."""

import pytest


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_aggregate_local_when_constraint_independent_masks(packdb_cli):
    """Each aggregate-local WHEN filters only its own aggregate term."""
    sql = """
        SELECT name, value, w1, w2, x FROM (
            VALUES ('a', 6, true, false),
                   ('b', 4, false, true),
                   ('c', 10, false, false)
        ) t(name, value, w1, w2)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN w1 + SUM(x * value) WHEN w2 <= 6
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    local_total = sum(
        r[ci["x"]] * r[ci["value"]]
        for r in rows
        if r[ci["w1"]] or r[ci["w2"]]
    )
    selected = {r[ci["name"]] for r in rows if r[ci["x"]] == 1}

    assert local_total <= 6
    assert "c" in selected, "Rows outside aggregate-local WHEN masks should not be constrained"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_aggregate_local_when_constraint_parenthesized_condition(packdb_cli):
    """Comparison predicates are supported when parenthesized in aggregate-local WHEN."""
    sql = """
        SELECT name, value, tier, x FROM (
            VALUES ('a', 7, 'high'),
                   ('b', 3, 'low'),
                   ('c', 9, 'none')
        ) t(name, value, tier)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN (tier = 'high') + SUM(x * value) WHEN (tier = 'low') <= 7
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    local_total = sum(
        r[ci["x"]] * r[ci["value"]]
        for r in rows
        if r[ci["tier"]] in {"high", "low"}
    )
    selected = {r[ci["name"]] for r in rows if r[ci["x"]] == 1}

    assert local_total <= 7
    assert "c" in selected


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_aggregate_local_when_objective_independent_masks(packdb_cli):
    """Aggregate-local WHEN works inside additive objective expressions."""
    sql = """
        SELECT name, value, bonus, w1, w2, x FROM (
            VALUES ('a', 10, 0, true, false),
                   ('b', 0, 9, false, true),
                   ('c', 8, 8, false, false)
        ) t(name, value, bonus, w1, w2)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * value) WHEN w1 + SUM(x * bonus) WHEN w2
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    selected = {r[ci["name"]] for r in rows if r[ci["x"]] == 1}
    assert selected == {"a", "b"}


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_expression_level_when_still_works(packdb_cli):
    """Existing whole-expression WHEN behavior remains available."""
    sql = """
        SELECT name, value, w1, x FROM (
            VALUES ('a', 6, true),
                   ('b', 4, true),
                   ('c', 10, false)
        ) t(name, value, w1)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) <= 6 WHEN w1
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    filtered_total = sum(r[ci["x"]] * r[ci["value"]] for r in rows if r[ci["w1"]])
    selected = {r[ci["name"]] for r in rows if r[ci["x"]] == 1}

    assert filtered_total <= 6
    assert "c" in selected


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error
@pytest.mark.error_binder
def test_expression_level_when_cannot_mix_with_aggregate_local_when(packdb_cli):
    """Expression-level WHEN and aggregate-local WHEN are rejected together."""
    sql = """
        SELECT name, value, w1, w2, x FROM (
            VALUES ('a', 6, true, false),
                   ('b', 4, false, true)
        ) t(name, value, w1, w2)
        DECIDE x IS BOOLEAN
        SUCH THAT (SUM(x * value) WHEN w1 + SUM(x * value) WHEN w2 <= 6) WHEN w1
        MAXIMIZE SUM(x * value)
    """
    packdb_cli.assert_error(sql, match=r"Cannot combine")


# ---------------------------------------------------------------------------
# A. Composition with other features
# ---------------------------------------------------------------------------


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.avg_rewrite
@pytest.mark.correctness
def test_aggregate_local_when_with_avg_constraint(packdb_cli):
    """AVG with aggregate-local WHEN uses filtered row count as denominator."""
    sql = """
        SELECT name, value, active, x FROM (
            VALUES ('a', 12, true),
                   ('b', 4, true),
                   ('c', 8, false),
                   ('d', 6, true)
        ) t(name, value, active)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) WHEN active <= 5
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    active_rows = [r for r in rows if r[ci["active"]]]
    active_sum = sum(r[ci["x"]] * r[ci["value"]] for r in active_rows)
    active_avg = active_sum / len(active_rows)
    assert active_avg <= 5.0 + 1e-6, f"AVG among active rows should be <= 5, got {active_avg}"

    # c is not active — unconstrained by this WHEN, should be selected
    c_row = [r for r in rows if r[ci["name"]] == "c"][0]
    assert c_row[ci["x"]] == 1, "Row outside aggregate-local WHEN should be unconstrained"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.xfail(reason="Aggregate-local WHEN + PER composition not yet supported")
def test_aggregate_local_when_with_per_constraint(packdb_cli):
    """Aggregate-local WHEN composes with PER for per-group filtered constraints."""
    sql = """
        SELECT name, value, grp, priority, x FROM (
            VALUES ('a', 10, 'X', true),
                   ('b', 5, 'X', false),
                   ('c', 8, 'Y', true),
                   ('d', 3, 'Y', false),
                   ('e', 7, 'X', true),
                   ('f', 6, 'Y', true)
        ) t(name, value, grp, priority)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN priority <= 12 PER grp
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    from collections import defaultdict
    groups = defaultdict(list)
    for r in rows:
        if r[ci["priority"]]:
            groups[r[ci["grp"]]].append(r)

    for grp_name, grp_rows in groups.items():
        grp_sum = sum(r[ci["x"]] * r[ci["value"]] for r in grp_rows)
        assert grp_sum <= 12 + 1e-6, f"SUM for priority rows in group '{grp_name}' should be <= 12, got {grp_sum}"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.avg_rewrite
@pytest.mark.per_clause
@pytest.mark.correctness
@pytest.mark.xfail(reason="Aggregate-local WHEN + PER composition not yet supported")
def test_aggregate_local_when_with_avg_and_per(packdb_cli):
    """AVG + aggregate-local WHEN + PER: triple composition."""
    sql = """
        SELECT name, value, grp, active, x FROM (
            VALUES ('a', 12, 'G1', true),
                   ('b', 4, 'G1', true),
                   ('c', 3, 'G1', false),
                   ('d', 10, 'G2', true),
                   ('e', 6, 'G2', true),
                   ('f', 2, 'G2', false)
        ) t(name, value, grp, active)
        DECIDE x IS BOOLEAN
        SUCH THAT AVG(x * value) WHEN active <= 6 PER grp
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    from collections import defaultdict
    groups = defaultdict(list)
    for r in rows:
        if r[ci["active"]]:
            groups[r[ci["grp"]]].append(r)

    for grp_name, grp_rows in groups.items():
        grp_sum = sum(r[ci["x"]] * r[ci["value"]] for r in grp_rows)
        grp_avg = grp_sum / len(grp_rows)
        assert grp_avg <= 6.0 + 1e-6, f"AVG for active rows in group '{grp_name}' should be <= 6, got {grp_avg}"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.count_rewrite
@pytest.mark.correctness
def test_aggregate_local_when_with_count(packdb_cli):
    """COUNT with aggregate-local WHEN: COUNT->SUM rewrite preserves filter."""
    sql = """
        SELECT name, value, active, x FROM (
            VALUES ('a', 10, true),
                   ('b', 5, true),
                   ('c', 8, false),
                   ('d', 3, true)
        ) t(name, value, active)
        DECIDE x IS BOOLEAN
        SUCH THAT COUNT(x) WHEN active <= 2
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    active_selected = sum(1 for r in rows if r[ci["active"]] and r[ci["x"]] == 1)
    assert active_selected <= 2, f"COUNT(x) among active rows should be <= 2, got {active_selected}"

    c_row = [r for r in rows if r[ci["name"]] == "c"][0]
    assert c_row[ci["x"]] == 1, "Non-active row should be unconstrained"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.min_max
@pytest.mark.correctness
@pytest.mark.xfail(reason="MIN/MAX easy-case rewrite does not carry aggregate-local WHEN filter")
def test_aggregate_local_when_with_max(packdb_cli):
    """MAX with aggregate-local WHEN: easy-case MAX strips to per-row within filter."""
    sql = """
        SELECT name, value, eligible, x FROM (
            VALUES ('a', 10, true),
                   ('b', 5, true),
                   ('c', 20, false),
                   ('d', 3, true)
        ) t(name, value, eligible)
        DECIDE x IS BOOLEAN
        SUCH THAT MAX(x * value) WHEN eligible <= 7
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    eligible_selected = [r for r in rows if r[ci["eligible"]] and r[ci["x"]] == 1]
    if eligible_selected:
        max_val = max(r[ci["value"]] for r in eligible_selected)
        assert max_val <= 7 + 1e-6, f"MAX(x*value) among eligible rows should be <= 7, got {max_val}"

    c_row = [r for r in rows if r[ci["name"]] == "c"][0]
    assert c_row[ci["x"]] == 1, "Non-eligible row with value=20 should be selected"


# ---------------------------------------------------------------------------
# B. Mixed terms
# ---------------------------------------------------------------------------


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_aggregate_local_when_mixed_filtered_unfiltered_constraint(packdb_cli):
    """Mixed constraint: one filtered aggregate + one unfiltered aggregate."""
    sql = """
        SELECT name, value, premium, x FROM (
            VALUES ('a', 8, true),
                   ('b', 5, false),
                   ('c', 10, true),
                   ('d', 3, false)
        ) t(name, value, premium)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN premium + SUM(x) <= 12
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    premium_sum = sum(r[ci["x"]] * r[ci["value"]] for r in rows if r[ci["premium"]])
    total_count = sum(r[ci["x"]] for r in rows)
    constraint_val = premium_sum + total_count
    assert constraint_val <= 12 + 1e-6, (
        f"SUM(x*value) WHEN premium + SUM(x) should be <= 12, got {constraint_val}"
    )


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_aggregate_local_when_objective_mixed_filtered_unfiltered(packdb_cli):
    """Objective with mixed filtered + unfiltered aggregates."""
    sql = """
        SELECT name, value, bonus, vip, x FROM (
            VALUES ('a', 10, 2, true),
                   ('b', 3, 8, false),
                   ('c', 7, 5, true),
                   ('d', 1, 1, false)
        ) t(name, value, bonus, vip)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * value) WHEN vip + SUM(x * bonus)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    selected = {r[ci["name"]] for r in rows if r[ci["x"]] == 1}
    assert len(selected) <= 2

    # Compute the actual objective for the selected set
    obj_val = sum(
        (r[ci["value"]] if r[ci["vip"]] else 0) + r[ci["bonus"]]
        for r in rows if r[ci["x"]] == 1
    )
    # a+c: vip_value=10+7=17, bonus=2+5=7, total=24 (best possible with SUM(x)<=2)
    assert obj_val >= 24 - 1e-6, f"Objective should be at least 24, got {obj_val}"


# ---------------------------------------------------------------------------
# C. Edge conditions
# ---------------------------------------------------------------------------


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.edge_case
@pytest.mark.correctness
def test_aggregate_local_when_all_filtered_out(packdb_cli):
    """One aggregate-local WHEN matches no rows — contributes zero, no crash."""
    sql = """
        SELECT name, value, flag, x FROM (
            VALUES ('a', 10, false),
                   ('b', 5, false),
                   ('c', 8, false)
        ) t(name, value, flag)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN flag + SUM(x * value) <= 23
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    total = sum(r[ci["x"]] * r[ci["value"]] for r in rows)
    assert total <= 23 + 1e-6
    # All three should be selected (10+5+8=23)
    selected = sum(1 for r in rows if r[ci["x"]] == 1)
    assert selected == 3, "All rows should be selected when dead WHEN term contributes 0"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_aggregate_local_when_overlapping_filters(packdb_cli):
    """Row matching multiple WHEN conditions contributes to both terms."""
    sql = """
        SELECT name, value, cat_a, cat_b, x FROM (
            VALUES ('a', 10, true, true),
                   ('b', 5, true, false),
                   ('c', 8, false, true),
                   ('d', 3, false, false)
        ) t(name, value, cat_a, cat_b)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN cat_a + SUM(x * value) WHEN cat_b <= 20
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    cat_a_sum = sum(r[ci["x"]] * r[ci["value"]] for r in rows if r[ci["cat_a"]])
    cat_b_sum = sum(r[ci["x"]] * r[ci["value"]] for r in rows if r[ci["cat_b"]])
    constraint_val = cat_a_sum + cat_b_sum
    assert constraint_val <= 20 + 1e-6, (
        f"SUM WHEN cat_a + SUM WHEN cat_b should be <= 20, got {constraint_val}"
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.edge_case
@pytest.mark.correctness
def test_aggregate_local_when_single_aggregate(packdb_cli):
    """Single aggregate with WHEN — degenerate case equivalent to expression-level."""
    sql = """
        SELECT name, value, active, x FROM (
            VALUES ('a', 10, true),
                   ('b', 5, true),
                   ('c', 8, false)
        ) t(name, value, active)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN active <= 10
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    active_sum = sum(r[ci["x"]] * r[ci["value"]] for r in rows if r[ci["active"]])
    assert active_sum <= 10 + 1e-6

    c_row = [r for r in rows if r[ci["name"]] == "c"][0]
    assert c_row[ci["x"]] == 1, "Unconstrained row should be selected"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.correctness
def test_aggregate_local_when_three_terms(packdb_cli):
    """Three additive aggregate terms with different WHEN conditions."""
    sql = """
        SELECT name, value, cat, x FROM (
            VALUES ('a', 10, 'X'),
                   ('b', 5, 'Y'),
                   ('c', 8, 'Z'),
                   ('d', 3, 'X'),
                   ('e', 7, 'Y')
        ) t(name, value, cat)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN (cat = 'X') + SUM(x * value) WHEN (cat = 'Y') + SUM(x * value) WHEN (cat = 'Z') <= 20
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    x_sum = sum(r[ci["x"]] * r[ci["value"]] for r in rows if r[ci["cat"]] == "X")
    y_sum = sum(r[ci["x"]] * r[ci["value"]] for r in rows if r[ci["cat"]] == "Y")
    z_sum = sum(r[ci["x"]] * r[ci["value"]] for r in rows if r[ci["cat"]] == "Z")
    total = x_sum + y_sum + z_sum
    assert total <= 20 + 1e-6, f"Sum of three filtered terms should be <= 20, got {total}"


# ---------------------------------------------------------------------------
# D. Error cases
# ---------------------------------------------------------------------------


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error
@pytest.mark.error_binder
def test_aggregate_local_when_decide_var_in_condition_error(packdb_cli):
    """DECIDE variable in aggregate-local WHEN condition is rejected."""
    sql = """
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5)
        ) t(name, value)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN x <= 10
        MAXIMIZE SUM(x * value)
    """
    packdb_cli.assert_error(sql, match=r"(?i)DECIDE variables")


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.error
@pytest.mark.error_binder
def test_aggregate_local_when_mixed_expression_objective_error(packdb_cli):
    """Expression-level + aggregate-local WHEN on objective is rejected."""
    sql = """
        SELECT name, value, w1, w2, x FROM (
            VALUES ('a', 10, true, false),
                   ('b', 5, false, true)
        ) t(name, value, w1, w2)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE (SUM(x * value) WHEN w1 + SUM(x * value) WHEN w2) WHEN w1
    """
    packdb_cli.assert_error(sql, match=r"Cannot combine")


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.error
@pytest.mark.error_binder
def test_aggregate_local_when_decide_var_in_objective_condition_error(packdb_cli):
    """DECIDE variable in objective aggregate-local WHEN condition is rejected."""
    sql = """
        SELECT name, value, x FROM (
            VALUES ('a', 10), ('b', 5)
        ) t(name, value)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * value) WHEN x
    """
    packdb_cli.assert_error(sql, match=r"(?i)DECIDE variables")


# ---------------------------------------------------------------------------
# E. Regressions
# ---------------------------------------------------------------------------


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_expression_level_when_objective_still_works(packdb_cli):
    """Expression-level WHEN on objective (not aggregate-local) still works."""
    sql = """
        SELECT name, value, vip, x FROM (
            VALUES ('a', 10, true),
                   ('b', 5, true),
                   ('c', 8, false)
        ) t(name, value, vip)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * value) WHEN vip
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    selected = {r[ci["name"]] for r in rows if r[ci["x"]] == 1}
    assert "a" in selected and "b" in selected, f"Should select VIP rows a and b, got {selected}"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
def test_expression_level_when_per_still_works(packdb_cli):
    """Expression-level WHEN + PER (not aggregate-local WHEN) still works."""
    sql = """
        SELECT name, value, grp, active, x FROM (
            VALUES ('a', 10, 'A', true),
                   ('b', 5, 'A', false),
                   ('c', 8, 'B', true),
                   ('d', 3, 'B', false)
        ) t(name, value, grp, active)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) <= 10 WHEN active PER grp
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    from collections import defaultdict
    groups = defaultdict(list)
    for r in rows:
        if r[ci["active"]]:
            groups[r[ci["grp"]]].append(r)

    for grp_name, grp_rows in groups.items():
        grp_sum = sum(r[ci["x"]] * r[ci["value"]] for r in grp_rows)
        assert grp_sum <= 10 + 1e-6, f"SUM for active rows in group '{grp_name}' should be <= 10, got {grp_sum}"

    # Non-active rows should be unconstrained
    for r in rows:
        if not r[ci["active"]]:
            assert r[ci["x"]] == 1, f"Non-active row '{r[ci['name']]}' should be selected"


# ---------------------------------------------------------------------------
# F. Compositions and grammar quirk
# ---------------------------------------------------------------------------


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_bilinear_aggregate_local_when_constraint(packdb_cli):
    """Bilinear product (b * x) with aggregate-local WHEN in constraint."""
    sql = """
        SELECT id, value, active, b, x FROM (
            VALUES (1, 10, true),
                   (2, 5, true),
                   (3, 8, false),
                   (4, 3, true)
        ) t(id, value, active)
        DECIDE b IS BOOLEAN, x IS BOOLEAN
        SUCH THAT SUM(b * x) WHEN active <= 1
        MAXIMIZE SUM(b * value + x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    active_both = sum(
        1 for r in rows
        if r[ci["active"]] and r[ci["b"]] == 1 and r[ci["x"]] == 1
    )
    assert active_both <= 1, f"SUM(b*x) among active rows should be <= 1, got {active_both}"


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_bilinear_aggregate_local_when_objective(packdb_cli):
    """Bilinear product with aggregate-local WHEN in objective."""
    sql = """
        SELECT id, value, premium, b, x FROM (
            VALUES (1, 10, true),
                   (2, 5, false),
                   (3, 8, true)
        ) t(id, value, premium)
        DECIDE b IS BOOLEAN, x IS BOOLEAN
        SUCH THAT SUM(b) <= 3 AND SUM(x) <= 3
        MAXIMIZE SUM(b * x * value) WHEN premium
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    # Only premium rows (1, 3) contribute to objective
    premium_obj = sum(
        r[ci["b"]] * r[ci["x"]] * r[ci["value"]]
        for r in rows if r[ci["premium"]]
    )
    # Best: b=1,x=1 for both premium rows => 10+8=18
    assert premium_obj >= 18 - 1e-6, f"Objective should be >= 18, got {premium_obj}"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_comparison
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.xfail(reason="NE indicator rewrite does not compose with aggregate-local WHEN filter")
def test_ne_aggregate_local_when_constraint(packdb_cli):
    """Not-equal (<>) with aggregate-local WHEN on constraint."""
    sql = """
        SELECT name, value, active, x FROM (
            VALUES ('a', 10, true),
                   ('b', 5, true),
                   ('c', 8, false),
                   ('d', 3, true)
        ) t(name, value, active)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) WHEN active <> 2
            AND SUM(x) <= 3
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    active_selected = sum(1 for r in rows if r[ci["active"]] and r[ci["x"]] == 1)
    assert active_selected != 2, f"SUM(x) among active rows must not be 2, got {active_selected}"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_between
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_between_aggregate_local_when_constraint(packdb_cli):
    """BETWEEN on aggregate with aggregate-local WHEN."""
    sql = """
        SELECT name, value, active, x FROM (
            VALUES ('a', 10, true),
                   ('b', 5, true),
                   ('c', 8, false),
                   ('d', 3, true)
        ) t(name, value, active)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN active BETWEEN 5 AND 13
        MAXIMIZE SUM(x * value)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    active_sum = sum(r[ci["x"]] * r[ci["value"]] for r in rows if r[ci["active"]])
    assert 5 - 1e-6 <= active_sum <= 13 + 1e-6, (
        f"SUM(x*value) among active rows should be in [5, 13], got {active_sum}"
    )
    # c (not active) should be unconstrained and selected
    c_row = [r for r in rows if r[ci["name"]] == "c"][0]
    assert c_row[ci["x"]] == 1, "Non-active row c should be selected"


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_entity_scoped_aggregate_local_when(packdb_cli):
    """Entity-scoped variable with aggregate-local WHEN on constraint."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, n.n_name, c.c_acctbal, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN IS BOOLEAN
        SUCH THAT SUM(keepN * c.c_acctbal) WHEN (c.c_acctbal > 5000) <= 50000
        MAXIMIZE SUM(keepN)
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    # Entity consistency: same nation => same keepN
    from collections import defaultdict
    nation_vals = defaultdict(set)
    for r in rows:
        nation_vals[r[ci["n_nationkey"]]].add(r[ci["keepN"]])
    for nk, vals in nation_vals.items():
        assert len(vals) == 1, f"Nation {nk} has inconsistent keepN values: {vals}"

    # Filtered constraint: among rows with acctbal > 5000, SUM(keepN * acctbal) <= 50000
    filtered_sum = sum(
        r[ci["keepN"]] * r[ci["c_acctbal"]]
        for r in rows if r[ci["c_acctbal"]] > 5000
    )
    assert filtered_sum <= 50000 + 1e-6, (
        f"SUM(keepN * acctbal) among high-balance rows should be <= 50000, got {filtered_sum}"
    )


@pytest.mark.when
@pytest.mark.when_constraint
@pytest.mark.error
def test_aggregate_local_when_unparenthesized_comparison_error(packdb_cli):
    """Unparenthesized comparison in aggregate-local WHEN condition errors in constraints."""
    sql = """
        SELECT name, value, tier, x FROM (
            VALUES ('a', 7, 'high'),
                   ('b', 3, 'low'),
                   ('c', 9, 'none')
        ) t(name, value, tier)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x * value) WHEN tier = 'high' <= 7
        MAXIMIZE SUM(x * value)
    """
    # Parser produces ((SUM(x*value) WHEN tier) = 'high') <= 7
    # which is a double comparison the binder rejects
    packdb_cli.assert_error(sql)


@pytest.mark.when
@pytest.mark.when_objective
@pytest.mark.correctness
def test_aggregate_local_when_objective_reassociation(packdb_cli):
    """Objective reassociator fixes unparenthesized comparison WHEN conditions."""
    sql = """
        SELECT name, value, tier, x FROM (
            VALUES ('a', 7, 'high'),
                   ('b', 3, 'low'),
                   ('c', 9, 'high')
        ) t(name, value, tier)
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) <= 2
        MAXIMIZE SUM(x * value) WHEN tier = 'high'
    """
    rows, cols = packdb_cli.execute(sql)
    ci = {name: i for i, name in enumerate(cols)}

    # ReassociateObjectiveWhenComparison transforms this to expression-level
    # WHEN(tier = 'high'). Only 'high' rows (a=7, c=9) contribute.
    # With SUM(x) <= 2, best to select a and c (obj = 16).
    selected = {r[ci["name"]] for r in rows if r[ci["x"]] == 1}
    assert "a" in selected and "c" in selected, (
        f"Should select high-tier rows a and c, got {selected}"
    )
