"""Shared fixtures and configuration for DECIDE tests.

Provides a CLI wrapper for the native packdb executable, a vanilla duckdb
connection for oracle data fetching (via dbgen-generated TPC-H data), an
oracle solver instance (with transparent result caching), and performance
tracking.

Architecture
------------
- **PackDB (DECIDE queries)**: native ``build/release/packdb`` executable
  invoked via subprocess (``PackDBCli``).  Reads ``packdb.db``.
- **Oracle (data fetching)**: vanilla ``duckdb`` Python package reading a
  separately generated TPC-H database (``_tpch_oracle.duckdb``).  The oracle
  database is created once per session via ``CALL dbgen(sf=0.01)`` and cached
  on disk.  This keeps the oracle completely independent of PackDB.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import duckdb
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from packdb_cli import PackDBCli
from solver.factory import get_solver
from oracle_cache import OracleCache, CachedOracleSolver
from performance.tracker import PerfTracker
from performance.reporter import print_perf_table

_TPCH_SF = 0.01

# ---------------------------------------------------------------------------
# Locate packdb.db and the packdb executable
# ---------------------------------------------------------------------------

_PACKDB_DB_CANDIDATES = [
    Path(__file__).resolve().parent.parent.parent / "packdb.db",
    Path(__file__).resolve().parent.parent.parent / "build" / "packdb.db",
]

_PACKDB_EXE_CANDIDATES = [
    Path(__file__).resolve().parent.parent.parent / "build" / "release" / "packdb",
]

_ORACLE_DB_PATH = Path(__file__).resolve().parent / "_tpch_oracle.duckdb"


def _find_packdb_db() -> Path | None:
    env = os.environ.get("PACKDB_DB_PATH")
    if env:
        p = Path(env)
        return p if p.exists() else None
    for p in _PACKDB_DB_CANDIDATES:
        if p.exists():
            return p
    return None


def _find_packdb_exe() -> Path | None:
    env = os.environ.get("PACKDB_EXE_PATH")
    if env:
        p = Path(env)
        return p if p.exists() else None
    for p in _PACKDB_EXE_CANDIDATES:
        if p.exists():
            return p
    return None


def _ensure_oracle_db() -> Path:
    """Generate the vanilla TPC-H database if it doesn't already exist."""
    if _ORACLE_DB_PATH.exists():
        return _ORACLE_DB_PATH
    conn = duckdb.connect(str(_ORACLE_DB_PATH))
    try:
        conn.execute("INSTALL tpch; LOAD tpch;")
        conn.execute(f"CALL dbgen(sf={_TPCH_SF})")
    finally:
        conn.close()
    return _ORACLE_DB_PATH


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


@pytest.fixture(scope="session")
def packdb_exe_path():
    """Path to the native packdb executable."""
    path = _find_packdb_exe()
    if path is None:
        pytest.skip(
            "packdb executable not found — build first or set PACKDB_EXE_PATH"
        )
    return str(path)


@pytest.fixture(scope="session")
def packdb_cli(packdb_exe_path, packdb_db_path):
    """Session-wide CLI wrapper for the native packdb executable.

    Queries are executed via subprocess, allowing multi-core execution.
    """
    return PackDBCli(packdb_exe_path, packdb_db_path)


@pytest.fixture(scope="session")
def _oracle_db_path():
    """Generate (once) and return the path to the vanilla TPC-H database."""
    return str(_ensure_oracle_db())


@pytest.fixture(scope="function")
def duckdb_conn(_oracle_db_path):
    """Per-test read-only connection to the vanilla TPC-H database."""
    conn = duckdb.connect(_oracle_db_path, read_only=True)
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
