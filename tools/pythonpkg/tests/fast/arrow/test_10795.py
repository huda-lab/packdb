import packdb
import pytest

pyarrow = pytest.importorskip('pyarrow')


@pytest.mark.parametrize('arrow_large_buffer_size', [True, False])
def test_10795(arrow_large_buffer_size):
    conn = packdb.connect()
    conn.sql(f"set arrow_large_buffer_size={arrow_large_buffer_size}")
    arrow = conn.sql("select map(['non-inlined string', 'test', 'packdb'], [42, 1337, 123]) as map").to_arrow_table()
    assert arrow.to_pydict() == {'map': [[('non-inlined string', 42), ('test', 1337), ('packdb', 123)]]}
