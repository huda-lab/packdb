import packdb
import pytest

pa = pytest.importorskip("pyarrow")
from packdb.typing import *


class TestArrowBufferSize(object):
    def test_arrow_buffer_size(self):
        con = packdb.connect()

        # All small string
        res = con.query("select 'bla'").arrow()
        assert res[0][0].type == pa.string()
        res = con.query("select 'bla'").record_batch()
        assert res.schema[0].type == pa.string()

        # All Large String
        con.execute("SET arrow_large_buffer_size=True")
        res = con.query("select 'bla'").arrow()
        assert res[0][0].type == pa.large_string()
        res = con.query("select 'bla'").record_batch()
        assert res.schema[0].type == pa.large_string()

        # All small string again
        con.execute("SET arrow_large_buffer_size=False")
        res = con.query("select 'bla'").arrow()
        assert res[0][0].type == pa.string()
        res = con.query("select 'bla'").record_batch()
        assert res.schema[0].type == pa.string()

    def test_arrow_buffer_size_udf(self):
        def just_return(x):
            return x

        con = packdb.connect()
        con.create_function('just_return', just_return, [VARCHAR], VARCHAR, type='arrow')

        res = con.query("select just_return('bla')").arrow()

        assert res[0][0].type == pa.string()

        # All Large String
        con.execute("SET arrow_large_buffer_size=True")

        res = con.query("select just_return('bla')").arrow()
        assert res[0][0].type == pa.large_string()
