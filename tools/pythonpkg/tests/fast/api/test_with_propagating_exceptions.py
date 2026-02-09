import pytest
import packdb


class TestWithPropagatingExceptions(object):
    def test_with(self):
        # Should propagate exception raised in the 'with packdb.connect() ..'
        with pytest.raises(packdb.ParserException, match="syntax error at or near *"):
            with packdb.connect() as con:
                print('before')
                con.execute('invalid')
                print('after')

        # Does not raise an exception
        with packdb.connect() as con:
            print('before')
            con.execute('select 1')
            print('after')
