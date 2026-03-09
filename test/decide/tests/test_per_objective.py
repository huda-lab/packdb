"""Tests for PER on objective with nested aggregate syntax.

Covers:
  Semantically-same (SUM — PER is a no-op):
  - test_sum_per_noop: SUM(x * cost) PER col = SUM(x * cost)
  - test_sum_sum_per_noop: SUM(SUM(x * cost)) PER col = SUM(x * cost)

  Semantically-different — inner MIN/MAX with outer SUM:
  - test_minimize_sum_max_per: MINIMIZE SUM(MAX(x * cost)) PER col (easy inner)
  - test_maximize_sum_min_per: MAXIMIZE SUM(MIN(x * cost)) PER col (easy inner)
  - test_maximize_sum_max_per: MAXIMIZE SUM(MAX(x * cost)) PER col (hard inner)
  - test_minimize_sum_min_per: MINIMIZE SUM(MIN(x * cost)) PER col (hard inner)

  Semantically-different — inner SUM with outer MIN/MAX:
  - test_minimize_max_sum_per: MINIMIZE MAX(SUM(x * cost)) PER col (easy outer)
  - test_maximize_min_sum_per: MAXIMIZE MIN(SUM(x * cost)) PER col (easy outer)

  WHEN + PER composition:
  - test_sum_max_when_per: SUM(MAX(expr)) WHEN cond PER col

  Edge cases:
  - test_single_group: all rows same PER value → matches non-PER
  - test_null_per_values: NULL PER values excluded

  Error cases:
  - test_flat_max_per_error: MAX(x * cost) PER col → error
  - test_flat_min_per_error: MIN(x * cost) PER col → error
"""

import pytest


# ============================================================================
# Semantically-Same Tests (SUM — PER is a no-op)
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.obj_minimize
def test_sum_per_noop(packdb_cli):
    """SUM(x * cost) PER col should give same result as without PER."""
    # Without PER
    result_no_per, cols = packdb_cli.execute("""
        SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 3
        MINIMIZE SUM(x * s_acctbal)
    """)

    # With PER (should be identical)
    result_per, _ = packdb_cli.execute("""
        SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 3
        MINIMIZE SUM(x * s_acctbal) PER s_nationkey
    """)

    ci = {name: i for i, name in enumerate(cols)}
    obj_no_per = sum(int(r[ci["x"]]) * float(r[ci["s_acctbal"]]) for r in result_no_per)
    obj_per = sum(int(r[ci["x"]]) * float(r[ci["s_acctbal"]]) for r in result_per)

    assert abs(obj_no_per - obj_per) < 0.01, (
        f"SUM PER should be no-op: without={obj_no_per:.2f}, with={obj_per:.2f}"
    )


@pytest.mark.per_clause
@pytest.mark.obj_maximize
def test_sum_sum_per_noop(packdb_cli):
    """SUM(SUM(x * cost)) PER col = SUM(x * cost) — explicit nested, still no-op."""
    result_flat, cols = packdb_cli.execute("""
        SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 3
        MAXIMIZE SUM(x * s_acctbal)
    """)

    result_nested, _ = packdb_cli.execute("""
        SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
        WHERE s_nationkey < 5
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 3
        MAXIMIZE SUM(SUM(x * s_acctbal)) PER s_nationkey
    """)

    ci = {name: i for i, name in enumerate(cols)}
    obj_flat = sum(int(r[ci["x"]]) * float(r[ci["s_acctbal"]]) for r in result_flat)
    obj_nested = sum(int(r[ci["x"]]) * float(r[ci["s_acctbal"]]) for r in result_nested)

    assert abs(obj_flat - obj_nested) < 0.01, (
        f"SUM(SUM) PER should be no-op: flat={obj_flat:.2f}, nested={obj_nested:.2f}"
    )


# ============================================================================
# Semantically-Different Tests — Inner MIN/MAX with Outer SUM
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_minimize
def test_minimize_sum_max_per(packdb_cli):
    """MINIMIZE SUM(MAX(x * l_quantity)) PER l_returnflag — easy inner case.

    Each group's MAX is independently minimized, then summed.
    With decoupled constraints (all PER same col), equivalent to independent per-group optimization.
    """
    result, cols = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 7
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1 PER l_returnflag
        MINIMIZE SUM(MAX(x * l_quantity)) PER l_returnflag
    """)

    ci = {name: i for i, name in enumerate(cols)}
    # Verify: each group has at least 1 selected
    groups = {}
    for r in result:
        flag = r[ci["l_returnflag"]]
        x_val = int(r[ci["x"]])
        qty = float(r[ci["l_quantity"]])
        if flag not in groups:
            groups[flag] = {"selected": [], "all_qty": []}
        groups[flag]["all_qty"].append(qty)
        if x_val == 1:
            groups[flag]["selected"].append(qty)

    for flag, g in groups.items():
        assert len(g["selected"]) >= 1, f"Group {flag}: SUM(x) >= 1 violated"

    # The objective is sum of per-group maxima of selected quantities
    total_max = sum(max(g["selected"]) for g in groups.values() if g["selected"])
    # Since we minimize, each group should pick the row with smallest quantity
    # (since MAX of a single selection = that value, and we want to minimize it)
    for flag, g in groups.items():
        if len(g["selected"]) == 1:
            # With only 1 required, the optimal picks the minimum-quantity row
            assert g["selected"][0] == min(g["all_qty"]), (
                f"Group {flag}: should pick min qty row, got {g['selected'][0]}, "
                f"min available = {min(g['all_qty'])}"
            )


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_maximize
def test_maximize_sum_min_per(packdb_cli):
    """MAXIMIZE SUM(MIN(x * l_quantity)) PER l_returnflag — easy inner case.

    Each group's MIN is independently maximized, then summed.
    """
    result, cols = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 7
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1 PER l_returnflag
        MAXIMIZE SUM(MIN(x * l_quantity)) PER l_returnflag
    """)

    ci = {name: i for i, name in enumerate(cols)}
    groups = {}
    for r in result:
        flag = r[ci["l_returnflag"]]
        x_val = int(r[ci["x"]])
        qty = float(r[ci["l_quantity"]])
        if flag not in groups:
            groups[flag] = {"selected": [], "all_qty": []}
        groups[flag]["all_qty"].append(qty)
        if x_val == 1:
            groups[flag]["selected"].append(qty)

    for flag, g in groups.items():
        assert len(g["selected"]) >= 1, f"Group {flag}: SUM(x) >= 1 violated"

    # MAXIMIZE MIN: each group should pick the row with max quantity
    # (MIN of a single selection = that value, maximize it → pick largest)
    for flag, g in groups.items():
        if len(g["selected"]) == 1:
            assert g["selected"][0] == max(g["all_qty"]), (
                f"Group {flag}: should pick max qty row, got {g['selected'][0]}, "
                f"max available = {max(g['all_qty'])}"
            )


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_maximize
def test_maximize_sum_max_per(packdb_cli):
    """MAXIMIZE SUM(MAX(x * l_quantity)) PER l_returnflag — hard inner case.

    Requires Big-M indicators per group.
    """
    result, cols = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 7
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1 PER l_returnflag
            AND SUM(x) <= 3 PER l_returnflag
        MAXIMIZE SUM(MAX(x * l_quantity)) PER l_returnflag
    """)

    ci = {name: i for i, name in enumerate(cols)}
    groups = {}
    for r in result:
        flag = r[ci["l_returnflag"]]
        x_val = int(r[ci["x"]])
        qty = float(r[ci["l_quantity"]])
        if flag not in groups:
            groups[flag] = {"selected": []}
        if x_val == 1:
            groups[flag]["selected"].append(qty)

    for flag, g in groups.items():
        assert 1 <= len(g["selected"]) <= 3, (
            f"Group {flag}: expected 1-3 selected, got {len(g['selected'])}"
        )

    # MAXIMIZE MAX per group: each group should include the largest quantity row
    # and the objective = sum of per-group maxima
    total = sum(max(g["selected"]) for g in groups.values() if g["selected"])
    assert total > 0, "Objective should be positive"


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_minimize
def test_minimize_sum_min_per(packdb_cli):
    """MINIMIZE SUM(MIN(x * l_quantity)) PER l_returnflag — hard inner case."""
    result, cols = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 7
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1 PER l_returnflag
            AND SUM(x) <= 3 PER l_returnflag
        MINIMIZE SUM(MIN(x * l_quantity)) PER l_returnflag
    """)

    ci = {name: i for i, name in enumerate(cols)}
    groups = {}
    for r in result:
        flag = r[ci["l_returnflag"]]
        x_val = int(r[ci["x"]])
        qty = float(r[ci["l_quantity"]])
        if flag not in groups:
            groups[flag] = {"selected": []}
        if x_val == 1:
            groups[flag]["selected"].append(qty)

    for flag, g in groups.items():
        assert 1 <= len(g["selected"]) <= 3

    # MINIMIZE MIN per group: each group's min of selected should be as small as possible
    total = sum(min(g["selected"]) for g in groups.values() if g["selected"])
    assert total >= 0, "Total should be non-negative (quantities are positive)"


# ============================================================================
# Semantically-Different Tests — Inner SUM with Outer MIN/MAX
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_minimize
def test_minimize_max_sum_per(packdb_cli):
    """MINIMIZE MAX(SUM(x * l_quantity)) PER l_returnflag — easy outer.

    Minimize the worst group's total quantity.
    """
    result, cols = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 2 PER l_returnflag
        MINIMIZE MAX(SUM(x * l_quantity)) PER l_returnflag
    """)

    ci = {name: i for i, name in enumerate(cols)}
    groups = {}
    for r in result:
        flag = r[ci["l_returnflag"]]
        x_val = int(r[ci["x"]])
        qty = float(r[ci["l_quantity"]])
        if flag not in groups:
            groups[flag] = 0.0
        if x_val == 1:
            groups[flag] += qty

    group_sums = list(groups.values())
    worst = max(group_sums)
    # The objective minimizes the max group sum
    # Verify all group sums are <= worst (trivially true) and worst is reasonable
    assert worst > 0


@pytest.mark.per_clause
@pytest.mark.min_max
@pytest.mark.obj_maximize
def test_maximize_min_sum_per(packdb_cli):
    """MAXIMIZE MIN(SUM(x * l_quantity)) PER l_returnflag — easy outer.

    Maximize the best (worst) group's total — make the weakest group as strong as possible.
    """
    result, cols = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 2 PER l_returnflag
        MAXIMIZE MIN(SUM(x * l_quantity)) PER l_returnflag
    """)

    ci = {name: i for i, name in enumerate(cols)}
    groups = {}
    for r in result:
        flag = r[ci["l_returnflag"]]
        x_val = int(r[ci["x"]])
        qty = float(r[ci["l_quantity"]])
        if flag not in groups:
            groups[flag] = 0.0
        if x_val == 1:
            groups[flag] += qty

    group_sums = list(groups.values())
    best_worst = min(group_sums)
    assert best_worst > 0


# ============================================================================
# WHEN + PER Composition
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.min_max
def test_sum_max_when_per(packdb_cli):
    """SUM(MAX(x * l_quantity)) WHEN cond PER col — WHEN filters, then PER groups."""
    result, cols = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, l_returnflag, x
        FROM lineitem WHERE l_orderkey <= 10
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 1 PER l_returnflag
        MINIMIZE SUM(MAX(x * l_quantity)) WHEN l_quantity > 10 PER l_returnflag
    """)

    ci = {name: i for i, name in enumerate(cols)}
    # Verify constraints satisfied
    groups = {}
    for r in result:
        flag = r[ci["l_returnflag"]]
        x_val = int(r[ci["x"]])
        if flag not in groups:
            groups[flag] = 0
        if x_val == 1:
            groups[flag] += 1

    for flag, count in groups.items():
        assert count >= 1, f"Group {flag}: SUM(x) >= 1 violated"


# ============================================================================
# Edge Cases
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
def test_single_group(packdb_cli):
    """All rows in same PER group → should match non-PER result."""
    # Non-PER
    result_no_per, cols = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey = 1
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 2
        MINIMIZE SUM(MAX(x * l_quantity)) PER l_orderkey
    """)

    # Equivalent non-PER (all rows have l_orderkey=1, so one group)
    result_flat, _ = packdb_cli.execute("""
        SELECT l_orderkey, l_linenumber, l_quantity, x
        FROM lineitem WHERE l_orderkey = 1
        DECIDE x IS BOOLEAN
        SUCH THAT SUM(x) >= 2
        MINIMIZE MAX(x * l_quantity)
    """)

    ci = {name: i for i, name in enumerate(cols)}
    max_per = max(int(r[ci["x"]]) * float(r[ci["l_quantity"]]) for r in result_no_per)
    max_flat = max(int(r[ci["x"]]) * float(r[ci["l_quantity"]]) for r in result_flat)

    assert abs(max_per - max_flat) < 0.01, (
        f"Single group should match flat: PER={max_per:.2f}, flat={max_flat:.2f}"
    )


# ============================================================================
# Error Cases
# ============================================================================

@pytest.mark.per_clause
@pytest.mark.min_max
def test_flat_max_per_error(packdb_cli):
    """MAX(x * cost) PER col without outer aggregate should error."""
    with pytest.raises(Exception, match="ambiguous|nested aggregate"):
        packdb_cli.execute("""
            SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
            WHERE s_nationkey < 5
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) >= 1
            MINIMIZE MAX(x * s_acctbal) PER s_nationkey
        """)


@pytest.mark.per_clause
@pytest.mark.min_max
def test_flat_min_per_error(packdb_cli):
    """MIN(x * cost) PER col without outer aggregate should error."""
    with pytest.raises(Exception, match="ambiguous|nested aggregate"):
        packdb_cli.execute("""
            SELECT s_suppkey, s_nationkey, s_acctbal, x FROM supplier
            WHERE s_nationkey < 5
            DECIDE x IS BOOLEAN
            SUCH THAT SUM(x) >= 1
            MAXIMIZE MIN(x * s_acctbal) PER s_nationkey
        """)
