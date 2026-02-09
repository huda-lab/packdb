import packdb


class TestModule:
    def test_paramstyle(self):
        assert packdb.paramstyle == "qmark"

    def test_threadsafety(self):
        assert packdb.threadsafety == 1

    def test_apilevel(self):
        assert packdb.apilevel == "2.0"
