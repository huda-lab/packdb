"""Shared fixtures and configuration for DECIDE tests.

Provides packdb/duckdb connections (with TPC-H data attached), an oracle
solver instance (with transparent result caching), and performance tracking.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

# Allow imports from the test/decide directory (solver, comparison, etc.)
sys.path.insert(0, str(Path(__file__).resolve().parent))

from solver.factory import get_solver
from oracle_cache import OracleCache, CachedOracleSolver
from performance.tracker import PerfTracker
from performance.reporter import print_perf_table

# ---------------------------------------------------------------------------
# Locate packdb.db
# ---------------------------------------------------------------------------

_PACKDB_DB_CANDIDATES = [
    Path(__file__).resolve().parent.parent.parent / "packdb.db",
    Path(__file__).resolve().parent.parent.parent / "build" / "packdb.db",
]


def _find_packdb_db() -> Path | None:
    env = os.environ.get("PACKDB_DB_PATH")
    if env:
        p = Path(env)
        return p if p.exists() else None
    for p in _PACKDB_DB_CANDIDATES:
        if p.exists():
            return p
    return None


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture(scope="session")
def packdb_db_path():
    """Path to the TPC-H database file.  Skips the session if not found."""
    path = _find_packdb_db()
    if path is None:
        pytest.skip("packdb.db not found — set PACKDB_DB_PATH or build first")
    return str(path)


@pytest.fixture(scope="function")
def packdb_conn(packdb_db_path):
    """In-memory PackDB connection with TPC-H data attached read-only."""
    import packdb
    conn = packdb.connect("")
    conn.execute(f"ATTACH '{packdb_db_path}' AS tpch (READ_ONLY)")
    conn.execute("SET search_path = 'tpch,main'")
    yield conn
    conn.close()


@pytest.fixture(scope="function")
def duckdb_conn(packdb_db_path):
    """In-memory connection for oracle data fetching (plain SQL, no DECIDE).

    Uses packdb rather than vanilla duckdb because both register the same
    pybind11 types and cannot coexist in one process.  Plain SQL works
    identically in both.
    """
    import packdb
    conn = packdb.connect("")
    conn.execute(f"ATTACH '{packdb_db_path}' AS tpch (READ_ONLY)")
    conn.execute("SET search_path = 'tpch,main'")
    yield conn
    conn.close()


@pytest.fixture(scope="session")
def _raw_oracle_solver():
    """Underlying ILP solver, or None if unavailable."""
    try:
        return get_solver()
    except ImportError:
        return None


@pytest.fixture(scope="session")
def _oracle_cache(packdb_db_path, request):
    """Session-wide oracle result cache.  GC runs only on full (unfiltered) runs."""
    cache = OracleCache(packdb_db_path)
    yield cache
    is_full_run = (
        not getattr(request.config.option, "markexpr", "")
        and not getattr(request.config.option, "keyword", "")
    )
    cache.save(gc=is_full_run)


@pytest.fixture(scope="function")
def oracle_solver(request, _raw_oracle_solver, _oracle_cache):
    """Cache-aware oracle solver for the current test.

    On first run (or after a test/db change) the real solver executes and
    the result is written to results/oracle_cache.json.  Subsequent runs
    return the cached value without invoking the solver at all.
    """
    if _raw_oracle_solver is None and _oracle_cache is None:
        pytest.skip("No ILP solver available and no oracle cache")
    return CachedOracleSolver(
        _raw_oracle_solver,
        request.node.nodeid,
        request.function,
        _oracle_cache,
    )


@pytest.fixture(scope="session")
def perf_tracker():
    """Session-wide performance tracker.  Saves JSON + prints table on teardown."""
    tracker = PerfTracker()
    yield tracker
    path = tracker.save_json()
    print_perf_table(tracker)
    if path:
        print(f"  Performance data saved to: {path}\n")
