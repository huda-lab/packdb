"""Parser-level syntax error tests.

These mirror the error cases from test/packdb/test_parser.test, ensuring that
DECIDE syntax errors are caught at parse time with clear error messages.
"""

import pytest

import packdb


@pytest.mark.error_parser
@pytest.mark.error
class TestParserErrors:
    """DECIDE parser should reject malformed syntax."""

    def test_missing_such_that(self, packdb_conn):
        """DECIDE x MAXIMIZE ... without SUCH THAT."""
        with pytest.raises(packdb.ParserException, match=r"syntax error.*MAXIMIZE"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_missing_decide_variable(self, packdb_conn):
        """DECIDE without a variable name."""
        with pytest.raises(packdb.ParserException, match=r"syntax error.*SUCH"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE SUCH THAT x IS BINARY
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)

    def test_missing_objective_expression(self, packdb_conn):
        """MAXIMIZE with no expression before LIMIT."""
        with pytest.raises(packdb.ParserException, match=r"syntax error"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x SUCH THAT x IS BINARY
                MAXIMIZE LIMIT 1
            """)

    def test_unknown_variable_type(self, packdb_conn):
        """IS CONTINUOUS is not a recognized variable type."""
        with pytest.raises(packdb.ParserException, match=r"syntax error.*CONTINUOUS"):
            packdb_conn.execute("""
                SELECT l_quantity FROM lineitem
                DECIDE x SUCH THAT x IS CONTINUOUS
                MAXIMIZE SUM(x*l_quantity) LIMIT 1
            """)
