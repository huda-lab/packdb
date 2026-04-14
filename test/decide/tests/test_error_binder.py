"""Binder-level semantic error tests.

These mirror the error cases from test/packdb/test_binder.test, updated to
match current PackDB behavior.  Some older error cases (SUM(x+x), strict <)
now succeed and are omitted.
"""

import pytest


@pytest.mark.error_binder
@pytest.mark.error
class TestBinderErrors:
    """DECIDE binder should reject semantically invalid queries."""

    def test_variable_conflicts_with_column(self, packdb_cli):
        """DECIDE variable name clashes with an existing column."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE l_quantity, x
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"conflicts with an existing column")

    def test_duplicate_decide_variables(self, packdb_cli):
        """Same variable name declared twice."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x, x
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"Duplicate DECIDE variable")

    def test_unknown_variable_in_constraint(self, packdb_cli):
        """Using an undeclared variable in IN expression."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE y
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"does not support IN on")

    def test_is_null_unsupported(self, packdb_cli):
        """IS NULL is not supported in SUCH THAT."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT x IS NULL
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"does not support")

    def test_sum_with_in_not_allowed(self, packdb_cli):
        """SUM(x) IN (...) is not a valid constraint form."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"does not support IN on")

    def test_non_decide_variable_in_constraint(self, packdb_cli):
        """Using a regular column (not a DECIDE var) as a constrained value."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT l_quantity <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"must be one of the DECIDE variables")

    def test_non_sum_avg_min_max_function_in_objective(self, packdb_cli):
        """STDDEV(x) is not supported in objective — only SUM, AVG, MIN, MAX, or COUNT is allowed."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) <= 5
                MAXIMIZE STDDEV(x*l_quantity) LIMIT 1
            """, match=r"only SUM, AVG, MIN, MAX, or COUNT is allowed")

    def test_no_decide_variable_in_sum(self, packdb_cli):
        """SUM over a regular column without DECIDE variable."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(l_quantity) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"must reference at least one DECIDE variable")

    def test_multiple_decide_variables_in_sum(self, packdb_cli):
        """Cubic (x*x*x) in SUM is not supported — must reject."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x*x*x) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"Triple.*products.*not supported")

    def test_nonlinear_decide_variables(self, packdb_cli):
        """SUM(col*(col+x)) passes the binder but fails at execution.

        This is a known issue — the binder doesn't catch this non-linearity,
        so it triggers an InternalException at execution time.
        """
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(l_quantity*(l_extendedprice+x)) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"Unsupported aggregate")

    def test_between_non_scalar(self, packdb_cli):
        """SUM BETWEEN with a non-scalar bound."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) BETWEEN l_quantity AND 1
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"not a scalar")

    def test_decide_between_decide_variable(self, packdb_cli):
        """Multi-variable per-row constraints are now supported (e.g. x BETWEEN y AND 1).
        This is needed for ABS linearization which generates constraints like d >= x - c."""
        # This used to be an error; now valid (x >= y AND x <= 1)
        result, cols = packdb_cli.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x, y
                SUCH THAT x BETWEEN y AND 1
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)
        assert len(result) > 0

    def test_in_rhs_with_decide_variable(self, packdb_cli):
        """IN domain constraints on DECIDE variables are not yet supported."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT x IN (1,2,x)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"IN domain constraints on DECIDE variables are not yet supported")

    def test_sum_rhs_non_scalar(self, packdb_cli):
        """SUM comparison RHS must be a scalar."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) <= l_quantity
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"not a scalar")

    def test_decide_variable_rhs_with_decide(self, packdb_cli):
        """Multi-variable per-row constraints are now supported (e.g. x <= y).
        This is needed for ABS linearization which generates constraints like d >= x - c."""
        # This used to be an error; now valid (x - y <= 0)
        # Use small dataset + SUM bound (otherwise MAXIMIZE is unbounded)
        result, cols = packdb_cli.execute("""
                SELECT l_quantity FROM lineitem
                WHERE l_orderkey <= 3
                DECIDE x, y
                SUCH THAT x <= y AND SUM(y) <= 100
                MAXIMIZE SUM(x*l_quantity)
            """)
        assert len(result) > 0

    def test_sum_equal_non_scalar(self, packdb_cli):
        """SUM = non-scalar expression."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) = l_quantity
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"not a scalar")

    def test_objective_with_addition(self, packdb_cli):
        """MAXIMIZE SUM(...)+3 is not supported."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) <= 3
                MAXIMIZE SUM(x*l_quantity)+3 LIMIT 1
            """, match=r"non-aggregate term")

    def test_objective_bare_column(self, packdb_cli):
        """MAXIMIZE with a bare column (not SUM) is not allowed."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) <= 3
                MAXIMIZE l_quantity LIMIT 1
            """, match=r"does not support")

    def test_subquery_rhs_non_scalar(self, packdb_cli):
        """Subquery RHS that references DECIDE variables."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) <= (SELECT l_quantity+x FROM lineitem)
                MAXIMIZE SUM(x) LIMIT 1
            """, match=r"not a scalar")

    # --- WHEN error cases ---

    @pytest.mark.when_constraint
    def test_when_decide_variable_in_condition(self, packdb_cli):
        """WHEN condition must not reference DECIDE variables."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x * l_quantity) <= 50 WHEN x = 1
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """, match=r"WHEN conditions cannot reference DECIDE variables")

    # --- COUNT error cases ---

    @pytest.mark.count_rewrite
    def test_count_integer_succeeds(self, packdb_cli):
        """COUNT(x) where x IS INTEGER should now succeed (indicator variable rewrite)."""
        rows, cols = packdb_cli.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT COUNT(x) >= 5
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """)
        assert len(rows) > 0

    @pytest.mark.count_rewrite
    def test_count_real_rejected(self, packdb_cli):
        """COUNT(x) where x IS REAL should be rejected."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x IS REAL
                SUCH THAT COUNT(x) >= 5
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """, match=r"COUNT.*requires a BOOLEAN or INTEGER")

    @pytest.mark.when_compound
    def test_when_decide_variable_in_compound_condition(self, packdb_cli):
        """WHEN compound condition must not hide a DECIDE variable."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT x <= 1 WHEN (x = 1 AND l_returnflag = 'R')
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """, match=r"WHEN conditions cannot reference DECIDE variables")

    # --- Correlated subquery on aggregate RHS error cases ---

    @pytest.mark.cons_subquery
    @pytest.mark.cons_aggregate
    def test_correlated_subquery_aggregate_rhs_non_scalar(self, packdb_cli):
        """Correlated subquery on aggregate constraint RHS produces per-row values — must error.

        SUM(x * supplycost) <= (SELECT p_size ...) decorrelates into a per-row
        column, but an aggregate constraint needs a single scalar RHS.
        """
        packdb_cli.assert_error("""
                SELECT ps_partkey, ps_suppkey, x
                FROM partsupp
                WHERE ps_partkey < 10
                DECIDE x IS INTEGER
                SUCH THAT SUM(x * ps_supplycost)
                          <= (SELECT CAST(p_size AS INTEGER) FROM part
                              WHERE p_partkey = ps_partkey)
                MAXIMIZE SUM(x)
            """, match=r"scalar right-hand side")

    @pytest.mark.cons_subquery
    @pytest.mark.cons_aggregate
    @pytest.mark.per_clause
    def test_correlated_subquery_aggregate_per_rhs_non_scalar(self, packdb_cli):
        """Correlated subquery on aggregate PER constraint RHS — must error.

        Same rejection as the non-PER case, but exercises the PER-path
        validation in ilp_model_builder.
        """
        packdb_cli.assert_error("""
                SELECT ps_partkey, ps_suppkey, x
                FROM partsupp
                WHERE ps_partkey < 20
                DECIDE x IS INTEGER
                SUCH THAT SUM(x) <= (SELECT CAST(p_size AS INTEGER) FROM part
                                     WHERE p_partkey = ps_partkey)
                          PER ps_suppkey
                MAXIMIZE SUM(x)
            """, match=r"scalar right-hand side")
