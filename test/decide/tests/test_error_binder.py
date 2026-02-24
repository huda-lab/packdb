"""Binder-level semantic error tests.

These mirror the error cases from test/packdb/test_binder.test, updated to
match current PackDB behavior.  Some older error cases (SUM(x+x), strict <)
now succeed and are omitted.
"""

import pytest

import packdb


@pytest.mark.error_binder
@pytest.mark.error
class TestBinderErrors:
    """DECIDE binder should reject semantically invalid queries."""

    def test_variable_conflicts_with_column(self, packdb_conn):
        """DECIDE variable name clashes with an existing column."""
        with pytest.raises(packdb.BinderException, match=r"conflicts with an existing column"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE l_quantity, x
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_duplicate_decide_variables(self, packdb_conn):
        """Same variable name declared twice."""
        with pytest.raises(packdb.BinderException, match=r"Duplicate DECIDE variable"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x, x
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_unknown_variable_in_constraint(self, packdb_conn):
        """Using an undeclared variable in IN expression."""
        with pytest.raises(packdb.BinderException, match=r"Only DECIDE variables are allowed for IN"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE y
                SUCH THAT x IN (1,2,3)
                MAXIMIZE SUM(x) LIMIT 1
            """)

    def test_is_null_unsupported(self, packdb_conn):
        """IS NULL is not supported in SUCH THAT."""
        with pytest.raises(packdb.BinderException, match=r"does not support"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT x IS NULL
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_sum_with_in_not_allowed(self, packdb_conn):
        """SUM(x) IN (...) is not a valid constraint form."""
        with pytest.raises(packdb.BinderException, match=r"Only DECIDE variables are allowed for IN"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) IN (1,2,3)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_non_decide_variable_in_constraint(self, packdb_conn):
        """Using a regular column (not a DECIDE var) as a constrained value."""
        with pytest.raises(packdb.BinderException, match=r"must be one of the DECIDE variables"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT l_quantity <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_non_sum_function_in_constraint(self, packdb_conn):
        """AVG(x) is not supported — only SUM is allowed."""
        with pytest.raises(packdb.BinderException, match=r"only SUM is allowed"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT AVG(x) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_no_decide_variable_in_sum(self, packdb_conn):
        """SUM over a regular column without DECIDE variable."""
        with pytest.raises(packdb.BinderException, match=r"must reference at least one DECIDE variable"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(l_quantity) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_multiple_decide_variables_in_sum(self, packdb_conn):
        """x*x in SUM is quadratic — must remain linear."""
        with pytest.raises(packdb.BinderException, match=r"must remain linear"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM((l_quantity+x)*x) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_nonlinear_decide_variables(self, packdb_conn):
        """SUM(col*(col+x)) passes the binder but fails at execution.

        This is a known issue — the binder doesn't catch this non-linearity,
        so it triggers an InternalException at execution time.
        """
        with pytest.raises(packdb.InternalException, match=r"Unsupported aggregate"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(l_quantity*(l_extendedprice+x)) <= 5
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_between_non_scalar(self, packdb_conn):
        """SUM BETWEEN with a non-scalar bound."""
        with pytest.raises(packdb.BinderException, match=r"not a scalar"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) BETWEEN l_quantity AND 1
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_decide_between_decide_variable(self, packdb_conn):
        """DECIDE variable cannot appear in BETWEEN bounds."""
        with pytest.raises(packdb.BinderException, match=r"cannot be compared.*DECIDE"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x, y
                SUCH THAT x BETWEEN y AND 1
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_in_rhs_with_decide_variable(self, packdb_conn):
        """IN list cannot contain DECIDE variables."""
        with pytest.raises(packdb.BinderException, match=r"cannot contain DECIDE"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT x IN (1,2,x)
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_sum_rhs_non_scalar(self, packdb_conn):
        """SUM comparison RHS must be a scalar."""
        with pytest.raises(packdb.BinderException, match=r"not a scalar"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) <= l_quantity
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_decide_variable_rhs_with_decide(self, packdb_conn):
        """DECIDE variable compared to another DECIDE variable."""
        with pytest.raises(packdb.BinderException, match=r"cannot be compared.*DECIDE"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x, y
                SUCH THAT x <= y
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_sum_equal_non_scalar(self, packdb_conn):
        """SUM = non-scalar expression."""
        with pytest.raises(packdb.BinderException, match=r"not a scalar"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) = l_quantity
                MAXIMIZE SUM(x) LIMIT 1
            """)

    def test_objective_with_addition(self, packdb_conn):
        """MAXIMIZE SUM(...)+3 is not supported."""
        with pytest.raises(packdb.BinderException, match=r"does not support.*only SUM"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) <= 3
                MAXIMIZE SUM(x*l_quantity)+3 LIMIT 1
            """)

    def test_objective_bare_column(self, packdb_conn):
        """MAXIMIZE with a bare column (not SUM) is not allowed."""
        with pytest.raises(packdb.BinderException, match=r"does not support"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) <= 3
                MAXIMIZE l_quantity LIMIT 1
            """)

    def test_subquery_rhs_non_scalar(self, packdb_conn):
        """Subquery RHS that references DECIDE variables."""
        with pytest.raises(packdb.BinderException, match=r"not a scalar"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x) <= (SELECT l_quantity+x FROM lineitem)
                MAXIMIZE SUM(x) LIMIT 1
            """)

    # --- WHEN error cases ---

    @pytest.mark.when_constraint
    def test_when_decide_variable_in_condition(self, packdb_conn):
        """WHEN condition must not reference DECIDE variables."""
        with pytest.raises(packdb.BinderException, match=r"WHEN conditions cannot reference DECIDE variables"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT SUM(x * l_quantity) <= 50 WHEN x = 1
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """)

    @pytest.mark.when_compound
    def test_when_decide_variable_in_compound_condition(self, packdb_conn):
        """WHEN compound condition must not hide a DECIDE variable."""
        with pytest.raises(packdb.BinderException, match=r"WHEN conditions cannot reference DECIDE variables"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x
                SUCH THAT x <= 1 WHEN (x = 1 AND l_returnflag = 'R')
                MAXIMIZE SUM(x * l_quantity) LIMIT 1
            """)
