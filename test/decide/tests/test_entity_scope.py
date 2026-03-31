"""Tests for table-scoped decision variables.

Covers:
  - Basic entity-scoped variable with oracle verification
  - Entity consistency: same entity → same variable value across join rows
  - IS INTEGER entity-scoped variable
  - Mixed row-scoped + entity-scoped variables (exercises VarIndexer three-block layout)
  - Entity-scoped with WHEN conditions
  - Error: scoping to nonexistent table
  - Entity-scoped with PER grouping constraints
  - Entity-scoped with COUNT rewrite
  - Entity-scoped with MAX constraint (MIN/MAX linearization)
  - Entity-scoped with AVG constraint (scaling)
  - Triple interaction: entity-scoped + WHEN + PER
"""

import pytest
from packdb_cli import PackDBCliError
from solver.types import VarType, ObjSense


# ---------------------------------------------------------------------------
# Test 1: Basic entity-scoped with oracle verification
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_nation_selection(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped variable on nation table: pick nations to maximize customer acctbal.

    Join customer x nation, decide keepN per nation (not per customer).
    Constraint: at most 100 join rows selected (SUM(keepN) <= 100).
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, n.n_name, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN IS BOOLEAN
        SUCH THAT SUM(keepN) <= 100
        MAXIMIZE SUM(keepN * c.c_acctbal)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    # Verify: rows with the same n_nationkey have the same keepN value
    nation_values = {}
    for row in packdb_result:
        nkey = int(row[1])
        keep = int(row[3])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Build oracle model: one variable per nation in region 0
    data = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT), CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
    """).fetchall()

    nation_ids = sorted(set(int(row[1]) for row in data))

    # Aggregated coefficients per nation (SQL semantics: SUM over join rows)
    nation_count = {}
    nation_acctbal = {}
    for row in data:
        nkey = int(row[1])
        acctbal = float(row[2])
        nation_count[nkey] = nation_count.get(nkey, 0) + 1
        nation_acctbal[nkey] = nation_acctbal.get(nkey, 0.0) + acctbal

    oracle_solver.create_model("entity_scope_nation")
    vnames = {nkey: f"keepN_{nkey}" for nkey in nation_ids}
    for nkey in nation_ids:
        oracle_solver.add_variable(vnames[nkey], VarType.BINARY)

    # SUM(keepN) <= 100 counts join rows, so each nation contributes its row count
    oracle_solver.add_constraint(
        {vnames[nkey]: float(nation_count[nkey]) for nkey in nation_ids},
        "<=", 100.0, name="nation_limit",
    )

    oracle_solver.set_objective(
        {vnames[nkey]: nation_acctbal[nkey] for nkey in nation_ids},
        ObjSense.MAXIMIZE,
    )

    result = oracle_solver.solve()
    oracle_obj = result.objective_value

    # Compute PackDB objective
    packdb_obj = 0.0
    for row in data:
        nkey = int(row[1])
        acctbal = float(row[2])
        packdb_obj += nation_values.get(nkey, 0) * acctbal

    assert abs(packdb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: PackDB={packdb_obj}, Oracle={oracle_obj}"


# ---------------------------------------------------------------------------
# Test 2: Entity consistency with tight constraint
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_consistency(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Verify rows from the same entity always have the same variable value.
    Use tight constraint to ensure some nations are excluded."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN IS BOOLEAN
        SUCH THAT SUM(keepN) <= 5
        MAXIMIZE SUM(keepN * c.c_acctbal)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    # Group by nation and verify consistency
    nation_values = {}
    for row in packdb_result:
        nkey = int(row[1])
        keep = int(row[2])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Inconsistent keepN for nation {nkey}: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify constraint holds (SUM counts join rows)
    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT), COUNT(*) as cnt
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        GROUP BY n.n_nationkey
    """).fetchall()

    total_selected_rows = 0
    for nkey_row in data:
        nkey = int(nkey_row[0])
        cnt = int(nkey_row[1])
        if nation_values.get(nkey, 0) == 1:
            total_selected_rows += cnt
    assert total_selected_rows <= 5, \
        f"Constraint violated: SUM(keepN) = {total_selected_rows} > 5"


# ---------------------------------------------------------------------------
# Test 3: IS INTEGER entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_integer(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped IS INTEGER variable — tests the INTEGER readback path with VarIndexer."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, qty
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND n.n_nationkey <= 5
        DECIDE n.qty IS INTEGER
        SUCH THAT qty <= 3
          AND SUM(qty) <= 10
        MAXIMIZE SUM(qty * c.c_acctbal)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    # Verify consistency: same nation → same qty
    nation_values = {}
    for row in packdb_result:
        nkey = int(row[1])
        q = int(row[2])
        if nkey in nation_values:
            assert nation_values[nkey] == q, \
                f"Nation {nkey} has inconsistent qty: {nation_values[nkey]} vs {q}"
        else:
            nation_values[nkey] = q

    # Verify per-row constraint: qty <= 3
    for nkey, q in nation_values.items():
        assert q <= 3, f"Nation {nkey} has qty={q} > 3"


# ---------------------------------------------------------------------------
# Test 4: Mixed row-scoped + entity-scoped (VarIndexer three-block layout)
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_mixed_with_row_scoped(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Mixed query: entity-scoped keepN (per nation) + row-scoped x (per customer row).
    This exercises the VarIndexer three-block layout where row-scoped and entity-scoped
    variables coexist with different indexing schemes."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, x, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_acctbal > 0
        DECIDE n.keepN IS BOOLEAN, x IS BOOLEAN
        SUCH THAT x <= keepN
          AND SUM(x) <= 10
        MAXIMIZE SUM(x * c.c_acctbal)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    # Verify entity consistency for keepN
    nation_values = {}
    for row in packdb_result:
        nkey = int(row[1])
        x_val = int(row[2])
        keep_val = int(row[3])
        if nkey in nation_values:
            assert nation_values[nkey] == keep_val, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep_val}"
        else:
            nation_values[nkey] = keep_val

        # Verify per-row constraint: x <= keepN
        assert x_val <= keep_val, \
            f"Per-row constraint violated: x={x_val} > keepN={keep_val} for custkey={row[0]}"

    # Verify SUM(x) <= 10
    total_x = sum(int(row[2]) for row in packdb_result)
    assert total_x <= 10, f"Aggregate constraint violated: SUM(x) = {total_x} > 10"

    # If a nation has keepN=0, all its customers must have x=0
    for row in packdb_result:
        nkey = int(row[1])
        x_val = int(row[2])
        if nation_values[nkey] == 0:
            assert x_val == 0, \
                f"Nation {nkey} has keepN=0 but customer {row[0]} has x={x_val}"


# ---------------------------------------------------------------------------
# Test 5: Entity-scoped with WHEN condition
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_with_when(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped variable with WHEN condition on constraint."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN IS BOOLEAN
        SUCH THAT SUM(keepN * c.c_acctbal) <= 50000 WHEN c.c_acctbal > 0
        MAXIMIZE SUM(keepN)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)

    # Verify entity consistency
    nation_values = {}
    for row in packdb_result:
        nkey = int(row[1])
        keep = int(row[2])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep


# ---------------------------------------------------------------------------
# Test 6: Error — nonexistent table
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_nonexistent_table(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Scoping to a nonexistent table should produce a clear error."""
    sql = """
        SELECT c.c_custkey, x
        FROM customer c
        DECIDE nonexistent.x IS BOOLEAN
        SUCH THAT SUM(x) <= 5
        MAXIMIZE SUM(x * c.c_acctbal)
    """
    with pytest.raises(PackDBCliError, match="not found"):
        packdb_cli.execute(sql)


# ---------------------------------------------------------------------------
# Test 7: Entity-scoped with PER grouping constraint
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.per_clause
def test_entity_scoped_with_per(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped variable with PER constraint on region.

    Tests VarIndexer entity dedup + PER group partitioning interaction.
    SUM(keepN) counts join rows, so each selected nation contributes its
    customer count to the per-region total.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, r.r_name, keepN
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        JOIN region r ON n.n_regionkey = r.r_regionkey
        DECIDE n.keepN IS BOOLEAN
        SUCH THAT SUM(keepN) <= 100 PER r_name
        MAXIMIZE SUM(keepN * c_acctbal)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    assert len(packdb_result) > 0

    nkey_idx = packdb_cols.index("n_nationkey")
    keep_idx = packdb_cols.index("keepN")
    region_idx = packdb_cols.index("r_name")

    # Verify entity consistency: same nation → same keepN
    nation_values = {}
    for row in packdb_result:
        nkey = int(row[nkey_idx])
        keep = int(row[keep_idx])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify PER constraint: SUM(keepN) <= 100 per region
    region_sums = {}
    for row in packdb_result:
        region = str(row[region_idx])
        keep = int(row[keep_idx])
        region_sums[region] = region_sums.get(region, 0) + keep

    for region, total in region_sums.items():
        assert total <= 100, \
            f"PER constraint violated: SUM(keepN) = {total} > 100 for region '{region}'"


# ---------------------------------------------------------------------------
# Test 8: Entity-scoped with COUNT rewrite
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.count_rewrite
def test_entity_scoped_with_count(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped BOOLEAN variable with COUNT constraint.

    COUNT(keepN) for BOOLEAN is rewritten to SUM(keepN). Tests the
    COUNT→SUM rewrite path with entity-scoped variable indexing.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN IS BOOLEAN
        SUCH THAT COUNT(keepN) >= 10
        MAXIMIZE SUM(keepN * c.c_acctbal)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    assert len(packdb_result) > 0

    # Verify entity consistency
    nation_values = {}
    for row in packdb_result:
        nkey = int(row[1])
        keep = int(row[2])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify COUNT constraint: total selected rows >= 10
    total_selected = sum(int(row[2]) for row in packdb_result)
    assert total_selected >= 10, \
        f"COUNT constraint violated: COUNT(keepN) = {total_selected} < 10"


# ---------------------------------------------------------------------------
# Test 9: Entity-scoped with MAX constraint (easy case)
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.min_max
def test_entity_scoped_with_max(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped variable with MAX(expr) <= K constraint (easy case).

    MAX(keepN * c.c_acctbal) <= 8000 means every customer in a selected
    nation must have acctbal <= 8000. Tests MIN/MAX linearization with
    entity-scoped variable indexing.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN IS BOOLEAN
        SUCH THAT MAX(keepN * c.c_acctbal) <= 8000
        MAXIMIZE SUM(keepN)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    assert len(packdb_result) > 0

    # Verify entity consistency
    nation_values = {}
    for row in packdb_result:
        nkey = int(row[1])
        keep = int(row[3])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify MAX constraint: for selected nations, all customer acctbal <= 8000
    for row in packdb_result:
        keep = int(row[3])
        acctbal = float(row[2])
        if keep == 1:
            assert acctbal <= 8000 + 1e-4, \
                f"MAX constraint violated: keepN=1 and c_acctbal={acctbal} > 8000"


# ---------------------------------------------------------------------------
# Test 10: Entity-scoped with AVG constraint
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.avg_rewrite
def test_entity_scoped_with_avg(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped variable with AVG constraint.

    AVG(keepN * c.c_acctbal) <= 3000 tests the AVG→SUM scaling
    with entity-scoped variables where the row count (N) differs from
    the entity count.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN IS BOOLEAN
        SUCH THAT AVG(keepN * c.c_acctbal) <= 3000
        MAXIMIZE SUM(keepN)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    assert len(packdb_result) > 0

    # Verify entity consistency
    nation_values = {}
    for row in packdb_result:
        nkey = int(row[1])
        keep = int(row[2])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify AVG constraint: SUM(keepN * acctbal) / N <= 3000
    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
    """).fetchall()

    n_total = len(data)
    weighted_sum = sum(
        nation_values.get(int(row[0]), 0) * float(row[1])
        for row in data
    )
    avg_val = weighted_sum / n_total
    assert avg_val <= 3000 + 1e-4, \
        f"AVG constraint violated: AVG(keepN * c_acctbal) = {avg_val:.2f} > 3000"


# ---------------------------------------------------------------------------
# Test 11: Triple interaction — entity_scope + WHEN + PER
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.per_clause
@pytest.mark.when_constraint
def test_entity_scoped_when_per_triple(packdb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Triple interaction: entity-scoped variable + WHEN + PER.

    SUM(keepN * c.c_acctbal) <= 50000 WHEN c.c_acctbal > 5000 PER r.r_name
    means: for each region, the sum of keepN * acctbal over high-balance
    customers only must be <= 50000.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, r.r_name, c.c_acctbal, keepN
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        JOIN region r ON n.n_regionkey = r.r_regionkey
        DECIDE n.keepN IS BOOLEAN
        SUCH THAT SUM(keepN * c_acctbal) <= 50000 WHEN c_acctbal > 5000 PER r_name
        MAXIMIZE SUM(keepN * c_acctbal)
    """
    packdb_result, packdb_cols = packdb_cli.execute(sql)
    assert len(packdb_result) > 0

    nkey_idx = packdb_cols.index("n_nationkey")
    keep_idx = packdb_cols.index("keepN")
    region_idx = packdb_cols.index("r_name")
    acctbal_idx = packdb_cols.index("c_acctbal")

    # Verify entity consistency
    nation_values = {}
    for row in packdb_result:
        nkey = int(row[nkey_idx])
        keep = int(row[keep_idx])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify WHEN+PER constraint: per region, sum over high-balance customers <= 50000
    region_sums = {}
    for row in packdb_result:
        acctbal = float(row[acctbal_idx])
        if acctbal > 5000:  # WHEN filter
            region = str(row[region_idx])
            keep = int(row[keep_idx])
            region_sums[region] = region_sums.get(region, 0.0) + keep * acctbal

    for region, total in region_sums.items():
        assert total <= 50000 + 1e-4, \
            f"WHEN+PER constraint violated: region '{region}' sum={total:.2f} > 50000"
