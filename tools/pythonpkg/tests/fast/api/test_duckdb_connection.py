import packdb
import pytest
from conftest import NumpyPandas, ArrowPandas

pa = pytest.importorskip("pyarrow")


def is_dunder_method(method_name: str) -> bool:
    if len(method_name) < 4:
        return False
    if method_name.startswith('_pybind11'):
        return True
    return method_name[:2] == '__' and method_name[:-3:-1] == '__'


@pytest.fixture(scope="session")
def tmp_database(tmp_path_factory):
    database = tmp_path_factory.mktemp("databases", numbered=True) / "tmp.packdb"
    return database


# This file contains tests for DuckDBPyConnection methods,
# wrapped by the 'packdb' module, to execute with the 'default_connection'
class TestDuckDBConnection(object):
    @pytest.mark.parametrize('pandas', [NumpyPandas(), ArrowPandas()])
    def test_append(self, pandas):
        packdb.execute("Create table integers (i integer)")
        df_in = pandas.DataFrame(
            {
                'numbers': [1, 2, 3, 4, 5],
            }
        )
        packdb.append('integers', df_in)
        assert packdb.execute('select count(*) from integers').fetchone()[0] == 5
        # cleanup
        packdb.execute("drop table integers")

    def test_default_connection_from_connect(self):
        packdb.sql('create or replace table connect_default_connect (i integer)')
        con = packdb.connect(':default:')
        con.sql('select i from connect_default_connect')
        packdb.sql('drop table connect_default_connect')
        with pytest.raises(packdb.Error):
            con.sql('select i from connect_default_connect')

        # not allowed with additional options
        with pytest.raises(
            packdb.InvalidInputException, match='Default connection fetching is only allowed without additional options'
        ):
            con = packdb.connect(':default:', read_only=True)

    def test_arrow(self):
        pyarrow = pytest.importorskip("pyarrow")
        packdb.execute("select [1,2,3]")
        result = packdb.arrow()

    def test_begin_commit(self):
        packdb.begin()
        packdb.execute("create table tbl as select 1")
        packdb.commit()
        res = packdb.table("tbl")
        packdb.execute("drop table tbl")

    def test_begin_rollback(self):
        packdb.begin()
        packdb.execute("create table tbl as select 1")
        packdb.rollback()
        with pytest.raises(packdb.CatalogException):
            # Table does not exist
            res = packdb.table("tbl")

    def test_cursor(self):
        packdb.execute("create table tbl as select 3")
        duckdb_cursor = packdb.cursor()
        res = duckdb_cursor.table("tbl").fetchall()
        assert res == [(3,)]
        duckdb_cursor.execute("drop table tbl")
        with pytest.raises(packdb.CatalogException):
            # 'tbl' no longer exists
            packdb.table("tbl")

    def test_cursor_lifetime(self):
        con = packdb.connect()

        def use_cursors():
            cursors = []
            for _ in range(10):
                cursors.append(con.cursor())

            for cursor in cursors:
                print("closing cursor")
                cursor.close()

        use_cursors()
        con.close()

    def test_df(self):
        ref = [([1, 2, 3],)]
        packdb.execute("select [1,2,3]")
        res_df = packdb.fetch_df()
        res = packdb.query("select * from res_df").fetchall()
        assert res == ref

    def test_duplicate(self):
        packdb.execute("create table tbl as select 5")
        dup_conn = packdb.duplicate()
        dup_conn.table("tbl").fetchall()
        packdb.execute("drop table tbl")
        with pytest.raises(packdb.CatalogException):
            dup_conn.table("tbl").fetchall()

    def test_readonly_properties(self):
        packdb.execute("select 42")
        description = packdb.description()
        rowcount = packdb.rowcount()
        assert description == [('42', 'NUMBER', None, None, None, None, None)]
        assert rowcount == -1

    def test_execute(self):
        assert [([4, 2],)] == packdb.execute("select [4,2]").fetchall()

    def test_executemany(self):
        # executemany does not keep an open result set
        # TODO: shouldn't we also have a version that executes a query multiple times with different parameters, returning all of the results?
        packdb.execute("create table tbl (i integer, j varchar)")
        packdb.executemany("insert into tbl VALUES (?, ?)", [(5, 'test'), (2, 'duck'), (42, 'quack')])
        res = packdb.table("tbl").fetchall()
        assert res == [(5, 'test'), (2, 'duck'), (42, 'quack')]
        packdb.execute("drop table tbl")

    def test_pystatement(self):
        with pytest.raises(packdb.ParserException, match='seledct'):
            statements = packdb.extract_statements('seledct 42; select 21')

        statements = packdb.extract_statements('select $1; select 21')
        assert len(statements) == 2
        assert statements[0].query == 'select $1'
        assert statements[0].type == packdb.StatementType.SELECT
        assert statements[0].named_parameters == set('1')
        assert statements[0].expected_result_type == [packdb.ExpectedResultType.QUERY_RESULT]

        assert statements[1].query == ' select 21'
        assert statements[1].type == packdb.StatementType.SELECT
        assert statements[1].named_parameters == set()

        with pytest.raises(
            packdb.InvalidInputException,
            match='Please provide either a DuckDBPyStatement or a string representing the query',
        ):
            rel = packdb.query(statements)

        with pytest.raises(packdb.BinderException, match="This type of statement can't be prepared!"):
            rel = packdb.query(statements[0])

        assert packdb.query(statements[1]).fetchall() == [(21,)]
        assert packdb.execute(statements[1]).fetchall() == [(21,)]

        with pytest.raises(
            packdb.InvalidInputException,
            match='Values were not provided for the following prepared statement parameters: 1',
        ):
            packdb.execute(statements[0])
        assert packdb.execute(statements[0], {'1': 42}).fetchall() == [(42,)]

        packdb.execute("create table tbl(a integer)")
        statements = packdb.extract_statements('insert into tbl select $1')
        assert statements[0].expected_result_type == [
            packdb.ExpectedResultType.CHANGED_ROWS,
            packdb.ExpectedResultType.QUERY_RESULT,
        ]
        with pytest.raises(
            packdb.InvalidInputException, match='executemany requires a non-empty list of parameter sets to be provided'
        ):
            packdb.executemany(statements[0])
        packdb.executemany(statements[0], [(21,), (22,), (23,)])
        assert packdb.table('tbl').fetchall() == [(21,), (22,), (23,)]
        packdb.execute("drop table tbl")

    def test_fetch_arrow_table(self):
        # Needed for 'fetch_arrow_table'
        pyarrow = pytest.importorskip("pyarrow")

        packdb.execute("Create Table test (a integer)")

        for i in range(1024):
            for j in range(2):
                packdb.execute("Insert Into test values ('" + str(i) + "')")
        packdb.execute("Insert Into test values ('5000')")
        packdb.execute("Insert Into test values ('6000')")
        sql = '''
        SELECT  a, COUNT(*) AS repetitions
        FROM    test
        GROUP BY a
        '''

        result_df = packdb.execute(sql).df()

        arrow_table = packdb.execute(sql).fetch_arrow_table()

        arrow_df = arrow_table.to_pandas()
        assert result_df['repetitions'].sum() == arrow_df['repetitions'].sum()
        packdb.execute("drop table test")

    def test_fetch_df(self):
        ref = [([1, 2, 3],)]
        packdb.execute("select [1,2,3]")
        res_df = packdb.fetch_df()
        res = packdb.query("select * from res_df").fetchall()
        assert res == ref

    def test_fetch_df_chunk(self):
        packdb.execute("CREATE table t as select range a from range(3000);")
        query = packdb.execute("SELECT a FROM t")
        cur_chunk = query.fetch_df_chunk()
        assert cur_chunk['a'][0] == 0
        assert len(cur_chunk) == 2048
        cur_chunk = query.fetch_df_chunk()
        assert cur_chunk['a'][0] == 2048
        assert len(cur_chunk) == 952
        packdb.execute("DROP TABLE t")

    def test_fetch_record_batch(self):
        # Needed for 'fetch_arrow_table'
        pyarrow = pytest.importorskip("pyarrow")

        packdb.execute("CREATE table t as select range a from range(3000);")
        packdb.execute("SELECT a FROM t")
        record_batch_reader = packdb.fetch_record_batch(1024)
        chunk = record_batch_reader.read_all()
        assert len(chunk) == 3000

    def test_fetchall(self):
        assert [([1, 2, 3],)] == packdb.execute("select [1,2,3]").fetchall()

    def test_fetchdf(self):
        ref = [([1, 2, 3],)]
        packdb.execute("select [1,2,3]")
        res_df = packdb.fetchdf()
        res = packdb.query("select * from res_df").fetchall()
        assert res == ref

    def test_fetchmany(self):
        assert [(0,), (1,)] == packdb.execute("select * from range(5)").fetchmany(2)

    def test_fetchnumpy(self):
        numpy = pytest.importorskip("numpy")
        packdb.execute("SELECT BLOB 'hello'")
        results = packdb.fetchall()
        assert results[0][0] == b'hello'

        packdb.execute("SELECT BLOB 'hello' AS a")
        results = packdb.fetchnumpy()
        assert results['a'] == numpy.array([b'hello'], dtype=object)

    def test_fetchone(self):
        assert (0,) == packdb.execute("select * from range(5)").fetchone()

    def test_from_arrow(self):
        assert None != packdb.from_arrow

    def test_from_csv_auto(self):
        assert None != packdb.from_csv_auto

    def test_from_df(self):
        assert None != packdb.from_df

    def test_from_parquet(self):
        assert None != packdb.from_parquet

    def test_from_query(self):
        assert None != packdb.from_query

    def test_get_table_names(self):
        assert None != packdb.get_table_names

    def test_install_extension(self):
        assert None != packdb.install_extension

    def test_load_extension(self):
        assert None != packdb.load_extension

    def test_query(self):
        assert [(3,)] == packdb.query("select 3").fetchall()

    def test_register(self):
        assert None != packdb.register

    def test_register_relation(self):
        con = packdb.connect()
        rel = con.sql('select [5,4,3]')
        con.register("relation", rel)

        con.sql("create table tbl as select * from relation")
        assert con.table('tbl').fetchall() == [([5, 4, 3],)]

    def test_unregister_problematic_behavior(self, duckdb_cursor):
        # We have a VIEW called 'vw' in the Catalog
        duckdb_cursor.execute("create temporary view vw as from range(100)")
        assert duckdb_cursor.execute("select * from vw").fetchone() == (0,)

        # Create a registered object called 'vw'
        arrow_result = duckdb_cursor.execute("select 42").arrow()
        with pytest.raises(packdb.CatalogException, match='View with name "vw" already exists'):
            duckdb_cursor.register('vw', arrow_result)

        # Temporary views take precedence over registered objects
        assert duckdb_cursor.execute("select * from vw").fetchone() == (0,)

        # Decide that we're done with this registered object..
        duckdb_cursor.unregister('vw')

        # This should not have affected the existing view:
        assert duckdb_cursor.execute("select * from vw").fetchone() == (0,)

    @pytest.mark.parametrize('pandas', [NumpyPandas(), ArrowPandas()])
    def test_relation_out_of_scope(self, pandas):
        def temporary_scope():
            # Create a connection, we will return this
            con = packdb.connect()
            # Create a dataframe
            df = pandas.DataFrame({'a': [1, 2, 3]})
            # The dataframe has to be registered as well
            # making sure it does not go out of scope
            con.register("df", df)
            rel = con.sql('select * from df')
            con.register("relation", rel)
            return con

        con = temporary_scope()
        res = con.sql('select * from relation').fetchall()
        print(res)

    def test_table(self):
        con = packdb.connect()
        con.execute("create table tbl as select 1")
        assert [(1,)] == con.table("tbl").fetchall()

    def test_table_function(self):
        assert None != packdb.table_function

    def test_unregister(self):
        assert None != packdb.unregister

    def test_values(self):
        assert None != packdb.values

    def test_view(self):
        packdb.execute("create view vw as select range(5)")
        assert [([0, 1, 2, 3, 4],)] == packdb.view("vw").fetchall()
        packdb.execute("drop view vw")

    def test_description(self):
        assert None != packdb.description

    def test_close(self):
        assert None != packdb.close

    def test_interrupt(self):
        assert None != packdb.interrupt

    def test_wrap_shadowing(self):
        pd = NumpyPandas()
        import packdb

        df = pd.DataFrame({"a": [1, 2, 3]})
        res = packdb.sql("from df").fetchall()
        assert res == [(1,), (2,), (3,)]

    def test_wrap_coverage(self):
        con = packdb.default_connection

        # Skip all of the initial __xxxx__ methods
        connection_methods = dir(con)
        filtered_methods = [method for method in connection_methods if not is_dunder_method(method)]
        for method in filtered_methods:
            # Assert that every method of DuckDBPyConnection is wrapped by the 'packdb' module
            assert method in dir(packdb)

    def test_connect_with_path(self, tmp_database):
        import pathlib

        assert isinstance(tmp_database, pathlib.Path)
        con = packdb.connect(tmp_database)
        assert con.sql("select 42").fetchall() == [(42,)]

        with pytest.raises(
            packdb.InvalidInputException, match="Please provide either a str or a pathlib.Path, not <class 'int'>"
        ):
            con = packdb.connect(5)

    def test_set_pandas_analyze_sample_size(self):
        con = packdb.connect(":memory:named", config={"pandas_analyze_sample": 0})
        res = con.sql("select current_setting('pandas_analyze_sample')").fetchone()
        assert res == (0,)

        # Find the cached config
        con2 = packdb.connect(":memory:named", config={"pandas_analyze_sample": 0})
        con2.execute(f"SET GLOBAL pandas_analyze_sample=2")

        # This change is reflected in 'con' because the instance was cached
        res = con.sql("select current_setting('pandas_analyze_sample')").fetchone()
        assert res == (2,)
