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
            """, match=r"Only DECIDE variables are allowed for IN")

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
            """, match=r"Only DECIDE variables are allowed for IN")

    def test_non_decide_variable_in_constraint(self, packdb_cli):
        """Using a regular column (not a DECIDE var) as a constrained value."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT l_quantity <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"must be one of the DECIDE variables")

    def test_non_sum_function_in_constraint(self, packdb_cli):
        """AVG(x) is not supported — only SUM is allowed."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT AVG(x) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"only SUM is allowed")

    def test_no_decide_variable_in_sum(self, packdb_cli):
        """SUM over a regular column without DECIDE variable."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(l_quantity) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"must reference at least one DECIDE variable")

    def test_multiple_decide_variables_in_sum(self, packdb_cli):
        """x*x in SUM is quadratic — must remain linear."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM((l_quantity+x)*x) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"must remain linear")

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
        """DECIDE variable cannot appear in BETWEEN bounds."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x, y
                SUCH THAT x BETWEEN y AND 1
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"cannot be compared.*DECIDE")

    def test_in_rhs_with_decide_variable(self, packdb_cli):
        """IN list cannot contain DECIDE variables."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT x IN (1,2,x)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"cannot contain DECIDE")

    def test_sum_rhs_non_scalar(self, packdb_cli):
        """SUM comparison RHS must be a scalar."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) <= l_quantity
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"not a scalar")

    def test_decide_variable_rhs_with_decide(self, packdb_cli):
        """DECIDE variable compared to another DECIDE variable."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x, y
                SUCH THAT x <= y
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """, match=r"cannot be compared.*DECIDE")

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
            """, match=r"does not support.*only SUM")

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

    @pytest.mark.when_compound
    def test_when_decide_variable_in_compound_condition(self, packdb_cli):
        """WHEN compound condition must not hide a DECIDE variable."""
        packdb_cli.assert_error("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT x <= 1 WHEN (x = 1 AND l_returnflag = 'R')
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """, match=r"WHEN conditions cannot reference DECIDE variables")
