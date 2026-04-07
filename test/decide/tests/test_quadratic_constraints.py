"""Tests for quadratic constraints (POWER(expr, 2) in SUCH THAT).

Covers QCQP: quadratic constraints with linear or quadratic objectives.
All tests that produce quadratic constraints use try/except since
HiGHS doesn't support quadratic constraints — only Gurobi does.
When HiGHS is the active solver, we verify the rejection error message.
"""

import functools
import re

import pytest

from packdb_cli import PackDBCliError


def _expect_gurobi(func):
    """Decorator: run test, accept HiGHS rejection as passing."""
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        try:
            return func(*args, **kwargs)
        except PackDBCliError as e:
            assert re.search(r"[Qq]uadratic|[Gg]urobi", str(e)), \
                f"Unexpected error (expected quadratic/Gurobi rejection): {e}"
    return wrapper


# ===================================================================
# Category 1: Core Correctness
# ===================================================================


@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintCorrectness:
    """Core correctness tests verifying Q matrix construction."""

    @_expect_gurobi
    def test_zero_budget_forces_exact_match(self, packdb_cli):
        """POWER(x - target, 2) <= 0 per-row forces x = target exactly."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 25.0 UNION ALL
                SELECT 3, 40.0
            )
            SELECT id, ROUND(x, 4) AS x, target
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND POWER(x - target, 2) <= 0
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")
        target_col = cols.index("target")

        for row in result:
            x_val = float(row[x_col])
            target = float(row[target_col])
            assert abs(x_val - target) < 0.01, \
                f"Row {row[id_col]}: zero-budget should force x={target}, got x={x_val}"

    @_expect_gurobi
    def test_aggregate_budget_with_linear_objective(self, packdb_cli):
        """MAXIMIZE SUM(x) SUCH THAT SUM(POWER(x - target, 2)) <= budget.

        Verify constraint satisfied and tighter budget → lower objective.
        """
        sql_tight = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x, target
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 3
            MAXIMIZE SUM(x)
        """
        tight_result, cols = packdb_cli.execute(sql_tight)
        x_col = cols.index("x")
        target_col = cols.index("target")

        tight_sum = sum(float(row[x_col]) for row in tight_result)
        tight_dev = sum((float(row[x_col]) - float(row[target_col]))**2
                        for row in tight_result)
        assert tight_dev <= 3.0 + 0.01, \
            f"Tight budget violated: sum of squared dev = {tight_dev}"

        sql_loose = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x, target
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 75
            MAXIMIZE SUM(x)
        """
        loose_result, cols2 = packdb_cli.execute(sql_loose)
        x_col2 = cols2.index("x")
        target_col2 = cols2.index("target")

        loose_sum = sum(float(row[x_col2]) for row in loose_result)
        loose_dev = sum((float(row[x_col2]) - float(row[target_col2]))**2
                        for row in loose_result)
        assert loose_dev <= 75.0 + 0.01, \
            f"Loose budget violated: sum of squared dev = {loose_dev}"
        assert loose_sum > tight_sum + 0.1, \
            f"Looser budget should allow higher objective: tight={tight_sum}, loose={loose_sum}"

    @_expect_gurobi
    def test_multi_variable_inner_expression(self, packdb_cli):
        """POWER(2*x + y - c, 2) <= K tests cross-terms (off-diagonal Q)."""
        sql = """
            WITH data AS (SELECT 1 AS id, 5.0 AS c)
            SELECT id, ROUND(x, 4) AS x, ROUND(y, 4) AS y
            FROM data
            DECIDE x IS REAL, y IS REAL
            SUCH THAT x >= 0 AND x <= 10
                AND y >= 0 AND y <= 10
                AND POWER(2*x + y - c, 2) <= 0
            MAXIMIZE SUM(x + y)
        """
        result, cols = packdb_cli.execute(sql)
        x_val = float(result[0][cols.index("x")])
        y_val = float(result[0][cols.index("y")])

        # POWER(2*x + y - 5, 2) <= 0 → 2*x + y = 5
        residual = abs(2 * x_val + y_val - 5.0)
        assert residual < 0.01, \
            f"Expected 2*x + y = 5, got 2*{x_val} + {y_val} = {2*x_val + y_val}"
        # MAXIMIZE x + y → maximize along 2x + y = 5
        # At x=0, y=5 → sum=5; at x=2.5, y=0 → sum=2.5
        assert abs(x_val) < 0.01 and abs(y_val - 5.0) < 0.01, \
            f"Expected x=0, y=5 for max x+y on 2x+y=5, got x={x_val}, y={y_val}"

    @_expect_gurobi
    def test_binding_vs_nonbinding(self, packdb_cli):
        """Large budget (non-binding) vs tight budget (binding) give different solutions."""
        sql_template = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= {budget}
            MAXIMIZE SUM(x)
        """
        big_result, cols = packdb_cli.execute(sql_template.format(budget=100000))
        big_sum = sum(float(row[cols.index("x")]) for row in big_result)

        tight_result, cols2 = packdb_cli.execute(sql_template.format(budget=3))
        tight_sum = sum(float(row[cols2.index("x")]) for row in tight_result)

        assert big_sum > tight_sum + 10, \
            f"Non-binding budget should allow much higher objective: big={big_sum}, tight={tight_sum}"

    @_expect_gurobi
    def test_negated_power(self, packdb_cli):
        """SUM(-POWER(x - t, 2)) >= -K equivalent to SUM(POWER(x - t, 2)) <= K."""
        sql_positive = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 50
            MAXIMIZE SUM(x)
        """
        sql_negated = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(-POWER(x - target, 2)) >= -50
            MAXIMIZE SUM(x)
        """
        pos_result, pos_cols = packdb_cli.execute(sql_positive)
        neg_result, neg_cols = packdb_cli.execute(sql_negated)

        pos_sum = sum(float(row[pos_cols.index("x")]) for row in pos_result)
        neg_sum = sum(float(row[neg_cols.index("x")]) for row in neg_result)
        assert abs(pos_sum - neg_sum) < 0.1, \
            f"Negated form should match: positive={pos_sum}, negated={neg_sum}"

    @_expect_gurobi
    def test_scaled_power(self, packdb_cli):
        """SUM(2 * POWER(x - t, 2)) <= 2*K gives same result as SUM(POWER(x - t, 2)) <= K."""
        sql_base = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 50
            MAXIMIZE SUM(x)
        """
        sql_scaled = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(2 * POWER(x - target, 2)) <= 100
            MAXIMIZE SUM(x)
        """
        base_result, base_cols = packdb_cli.execute(sql_base)
        scaled_result, scaled_cols = packdb_cli.execute(sql_scaled)

        base_sum = sum(float(row[base_cols.index("x")]) for row in base_result)
        scaled_sum = sum(float(row[scaled_cols.index("x")]) for row in scaled_result)
        assert abs(base_sum - scaled_sum) < 0.1, \
            f"Scaled form should match: base={base_sum}, scaled={scaled_sum}"

    @_expect_gurobi
    def test_data_dependent_coefficients(self, packdb_cli):
        """SUM(POWER(weight * x - target, 2)) <= K with varying weight per row.

        Weight is inside the POWER inner expression, so it naturally
        participates in the outer-product Q construction.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 2.0 AS weight, 10.0 AS target UNION ALL
                SELECT 2, 1.0, 5.0 UNION ALL
                SELECT 3, 3.0, 15.0
            )
            SELECT id, ROUND(x, 4) AS x, weight, target
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(weight * x - target, 2)) <= 0
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        w_col = cols.index("weight")
        t_col = cols.index("target")

        for row in result:
            x_val = float(row[x_col])
            weight = float(row[w_col])
            target = float(row[t_col])
            expected = target / weight
            assert abs(x_val - expected) < 0.01, \
                f"Expected x = target/weight = {expected}, got {x_val}"


# ===================================================================
# Category 2: Syntax Variants & Non-POWER Forms
# ===================================================================


@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintSyntax:
    """All syntax forms should produce identical results."""

    def _run_syntax_variant(self, packdb_cli, constraint_expr):
        """Helper: run the same problem with different quadratic syntax."""
        sql = f"""
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM({constraint_expr}) <= 50
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        return sum(float(row[cols.index("x")]) for row in result)

    @_expect_gurobi
    def test_power_form(self, packdb_cli):
        """POWER() form."""
        val = self._run_syntax_variant(packdb_cli, "POWER(x - target, 2)")
        assert val > 60  # Should be above sum of targets

    @_expect_gurobi
    def test_starstar_form(self, packdb_cli):
        """** operator form matches POWER form."""
        power_val = self._run_syntax_variant(packdb_cli, "POWER(x - target, 2)")
        starstar_val = self._run_syntax_variant(packdb_cli, "(x - target) ** 2")
        assert abs(power_val - starstar_val) < 0.1

    @_expect_gurobi
    def test_self_multiply_form(self, packdb_cli):
        """(expr)*(expr) form matches POWER form."""
        power_val = self._run_syntax_variant(packdb_cli, "POWER(x - target, 2)")
        mult_val = self._run_syntax_variant(packdb_cli, "(x - target) * (x - target)")
        assert abs(power_val - mult_val) < 0.1

    @_expect_gurobi
    def test_bare_self_product(self, packdb_cli):
        """SUM(x * x) <= K — simplest self-product, no POWER syntax."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2 UNION ALL SELECT 3
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(x * x) <= 75
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        x_vals = [float(row[x_col]) for row in result]
        sum_sq = sum(v * v for v in x_vals)
        assert sum_sq <= 75.01, f"SUM(x*x) = {sum_sq} exceeds budget 75"
        assert abs(sum(x_vals) - 15.0) < 0.1, f"Expected SUM(x) ≈ 15, got {sum(x_vals)}"

    @_expect_gurobi
    def test_mixed_self_product_and_bilinear(self, packdb_cli):
        """SUM(x * x + x * y + y * y) <= K — self-products + bilinear cross-term."""
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, ROUND(x, 4) AS x, ROUND(y, 4) AS y
            FROM data
            DECIDE x IS REAL, y IS REAL
            SUCH THAT x >= 0 AND x <= 10
                AND y >= 0 AND y <= 10
                AND SUM(x * x + x * y + y * y) <= 12
            MAXIMIZE SUM(x + y)
        """
        result, cols = packdb_cli.execute(sql)
        x_val = float(result[0][cols.index("x")])
        y_val = float(result[0][cols.index("y")])
        qform = x_val**2 + x_val * y_val + y_val**2
        assert qform <= 12.01, f"Quadratic form = {qform} exceeds budget 12"

    @_expect_gurobi
    def test_constant_scaled_power(self, packdb_cli):
        """SUM(3 * POWER(x, 2)) <= K with constant coefficient."""
        sql = """
            WITH data AS (
                SELECT 1 AS id UNION ALL SELECT 2
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(3 * POWER(x, 2)) <= 75
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        x_vals = [float(row[x_col]) for row in result]
        scaled_sq = sum(3 * v * v for v in x_vals)
        assert scaled_sq <= 75.01, f"SUM(3*x^2) = {scaled_sq} exceeds budget 75"


# ===================================================================
# Category 3: Feature Interactions
# ===================================================================


@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintInteractions:
    """POWER constraints combined with WHEN, PER, and other constraints."""

    @_expect_gurobi
    def test_when_filtering(self, packdb_cli):
        """SUM(POWER(x - t, 2)) <= K WHEN active = 1.

        Only active rows count. Inactive rows free to maximize.
        """
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target, 1 AS active UNION ALL
                SELECT 2, 20.0, 1 UNION ALL
                SELECT 3, 30.0, 0
            )
            SELECT id, ROUND(x, 4) AS x, target, active
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 2 WHEN active = 1
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")
        active_col = cols.index("active")
        target_col = cols.index("target")

        for row in result:
            rid = int(row[id_col])
            x_val = float(row[x_col])
            is_active = int(row[active_col])
            if is_active == 0:
                assert x_val > 90, f"Inactive row {rid} should be at upper bound, got {x_val}"

        active_dev = sum(
            (float(row[x_col]) - float(row[target_col]))**2
            for row in result if int(row[active_col]) == 1
        )
        assert active_dev <= 2.01, f"Active deviation {active_dev} exceeds budget 2"

    @_expect_gurobi
    def test_per_group_quadratic_constraint(self, packdb_cli):
        """SUM(POWER(x - target, 2)) <= K PER group — one quadratic constraint per group."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 'A' AS grp, 10.0 AS target UNION ALL
                SELECT 2, 'A', 15.0 UNION ALL
                SELECT 3, 'B', 30.0 UNION ALL
                SELECT 4, 'B', 35.0
            )
            SELECT id, grp, ROUND(x, 4) AS x, target
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 10 PER grp
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        grp_col = cols.index("grp")
        target_col = cols.index("target")

        # Check per-group deviation budgets
        from collections import defaultdict
        group_dev = defaultdict(float)
        for row in result:
            x_val = float(row[x_col])
            target = float(row[target_col])
            group_dev[row[grp_col]] += (x_val - target) ** 2

        for grp, dev in group_dev.items():
            assert dev <= 10.01, \
                f"Group {grp}: SUM(POWER(x-target,2)) = {dev} exceeds budget 10"

    @_expect_gurobi
    def test_multiple_quadratic_constraints(self, packdb_cli):
        """Two quadratic constraints simultaneously."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS t1, 15.0 AS t2 UNION ALL
                SELECT 2, 20.0, 25.0 UNION ALL
                SELECT 3, 30.0, 35.0
            )
            SELECT id, ROUND(x, 4) AS x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - t1, 2)) <= 50
                AND SUM(POWER(x - t2, 2)) <= 50
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        id_col = cols.index("id")
        assert len(result) == 3

        # Both constraints must be satisfied
        t1_map = {1: 10.0, 2: 20.0, 3: 30.0}
        t2_map = {1: 15.0, 2: 25.0, 3: 35.0}
        dev1 = sum((float(r[x_col]) - t1_map[int(r[id_col])])**2 for r in result)
        dev2 = sum((float(r[x_col]) - t2_map[int(r[id_col])])**2 for r in result)
        assert dev1 <= 50.01, f"First quadratic constraint violated: {dev1}"
        assert dev2 <= 50.01, f"Second quadratic constraint violated: {dev2}"

    @_expect_gurobi
    def test_qcqp_quadratic_objective_and_constraint(self, packdb_cli):
        """Genuine QCQP: quadratic in both objective and constraint."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS preferred, 20.0 AS required UNION ALL
                SELECT 2, 50.0, 30.0 UNION ALL
                SELECT 3, 30.0, 25.0
            )
            SELECT id, ROUND(x, 4) AS x, preferred, required
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - required, 2)) <= 50
            MINIMIZE SUM(POWER(x - preferred, 2))
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        req_col = cols.index("required")

        req_dev = sum((float(row[x_col]) - float(row[req_col]))**2
                      for row in result)
        assert req_dev <= 50.01, f"Required constraint violated: dev = {req_dev}"
        assert len(result) == 3

    @_expect_gurobi
    def test_mixed_linear_and_quadratic_constraints(self, packdb_cli):
        """SUM(x) >= 50 AND SUM(POWER(x - t, 2)) <= K."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 15.0 UNION ALL
                SELECT 3, 20.0
            )
            SELECT id, ROUND(x, 4) AS x, target
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(x) >= 50
                AND SUM(POWER(x - target, 2)) <= 25
            MINIMIZE SUM(POWER(x - target, 2))
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        target_col = cols.index("target")

        total_x = sum(float(row[x_col]) for row in result)
        assert total_x >= 49.99, f"Linear constraint violated: SUM(x) = {total_x}"

        total_dev = sum((float(row[x_col]) - float(row[target_col]))**2
                        for row in result)
        assert total_dev <= 25.01, f"Quadratic constraint violated: dev = {total_dev}"


# ===================================================================
# Category 4: Variable Types
# ===================================================================


@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintVarTypes:
    """Quadratic constraints with different variable types."""

    @_expect_gurobi
    def test_real_variables(self, packdb_cli):
        """Standard continuous QP constraint."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0
            )
            SELECT id, ROUND(x, 4) AS x, target
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 10
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        target_col = cols.index("target")
        dev = sum((float(row[x_col]) - float(row[target_col]))**2
                  for row in result)
        assert dev <= 10.01

    @_expect_gurobi
    def test_integer_variables(self, packdb_cli):
        """MIQP with quadratic constraint — integer solutions must satisfy."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10 AS target UNION ALL
                SELECT 2, 20 UNION ALL
                SELECT 3, 30
            )
            SELECT id, x, target
            FROM data
            DECIDE x IS INTEGER
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 6
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        target_col = cols.index("target")

        dev = sum((int(row[x_col]) - int(row[target_col]))**2 for row in result)
        assert dev <= 6, f"Integer solution violates quadratic constraint: dev = {dev}"

    @_expect_gurobi
    def test_table_scoped_variables(self, packdb_cli):
        """Table-scoped variable with quadratic constraint."""
        sql = """
            WITH items AS (
                SELECT 'A' AS item, 10.0 AS target UNION ALL
                SELECT 'A', 12.0 UNION ALL
                SELECT 'B', 20.0 UNION ALL
                SELECT 'B', 22.0
            )
            SELECT item, ROUND(x, 4) AS x, target
            FROM items
            DECIDE items.x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND SUM(POWER(x - target, 2)) <= 20
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        item_col = cols.index("item")
        target_col = cols.index("target")

        item_vals = {}
        for row in result:
            item = row[item_col]
            x_val = float(row[x_col])
            if item in item_vals:
                assert abs(x_val - item_vals[item]) < 0.01, \
                    f"Table-scoped var: item {item} has inconsistent x values"
            item_vals[item] = x_val

        dev = sum((float(row[x_col]) - float(row[target_col]))**2
                  for row in result)
        assert dev <= 20.01, f"Quadratic constraint violated: dev = {dev}"


# ===================================================================
# Category 5: Edge Cases
# ===================================================================


@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintEdgeCases:
    """Edge cases and boundary conditions."""

    def test_infeasible_negative_budget(self, packdb_cli):
        """POWER(x, 2) <= -1 is impossible — should report infeasibility or rejection."""
        sql = """
            WITH data AS (SELECT 1 AS id)
            SELECT id, x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 10
                AND POWER(x, 2) <= -1
            MAXIMIZE SUM(x)
        """
        with pytest.raises(PackDBCliError):
            packdb_cli.execute(sql)

    @_expect_gurobi
    def test_single_row(self, packdb_cli):
        """POWER constraint with single row degenerates to simple bound."""
        sql = """
            WITH data AS (SELECT 1 AS id, 5.0 AS target)
            SELECT id, ROUND(x, 4) AS x, target
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND POWER(x - target, 2) <= 4
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_val = float(result[0][cols.index("x")])
        # (x - 5)^2 <= 4 → x ∈ [3, 7], maximize → x = 7
        assert abs(x_val - 7.0) < 0.01, f"Expected x=7, got {x_val}"

    @_expect_gurobi
    def test_per_row_constraint(self, packdb_cli):
        """POWER(x - target, 2) <= K without SUM — per-row constraint."""
        sql = """
            WITH data AS (
                SELECT 1 AS id, 10.0 AS target UNION ALL
                SELECT 2, 20.0 UNION ALL
                SELECT 3, 30.0
            )
            SELECT id, ROUND(x, 4) AS x, target
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 100
                AND POWER(x - target, 2) <= 9
            MAXIMIZE SUM(x)
        """
        result, cols = packdb_cli.execute(sql)
        x_col = cols.index("x")
        target_col = cols.index("target")
        id_col = cols.index("id")

        for row in result:
            x_val = float(row[x_col])
            target = float(row[target_col])
            dev_sq = (x_val - target) ** 2
            assert dev_sq <= 9.01, \
                f"Row {row[id_col]}: (x-target)^2 = {dev_sq} exceeds 9"
            # MAXIMIZE SUM(x) → x = target + 3 for each row
            assert abs(x_val - (target + 3.0)) < 0.01, \
                f"Row {row[id_col]}: expected x={target + 3}, got {x_val}"


# ===================================================================
# Category 6: Error Handling
# ===================================================================


@pytest.mark.correctness
@pytest.mark.quadratic
class TestQuadraticConstraintErrors:
    """Error cases and solver rejection."""

    def test_highs_rejection(self, packdb_cli):
        """HiGHS should reject quadratic constraints with a clear error."""
        sql = """
            WITH data AS (SELECT 1 AS id, 10.0 AS target)
            SELECT id, x
            FROM data
            DECIDE x IS REAL
            SUCH THAT x >= 0 AND x <= 10
                AND SUM(POWER(x - target, 2)) <= 5
            MAXIMIZE SUM(x)
        """
        try:
            packdb_cli.execute(sql)
            pytest.skip("Gurobi is available; HiGHS rejection test not applicable")
        except PackDBCliError as e:
            assert re.search(r"[Qq]uadratic|[Gg]urobi", str(e)), \
                f"Expected quadratic/Gurobi rejection, got: {e}"
