#!/usr/bin/env python3
"""Generate TPC-H databases at multiple scale factors for benchmarking.

Usage:
    python3 benchmark/decide/generate_databases.py
    make decide-bench-setup
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
DATABASES_DIR = SCRIPT_DIR / "databases"
PACKDB_EXE = REPO_ROOT / "build" / "release" / "packdb"

DB_SIZES: dict[str, float] = {
    "small": 0.01,
    "medium": 0.05,
    "large": 0.2,
}


def main() -> None:
    if not PACKDB_EXE.exists():
        print(f"ERROR: packdb executable not found at {PACKDB_EXE}", file=sys.stderr)
        print("Run 'make release' first.", file=sys.stderr)
        sys.exit(1)

    DATABASES_DIR.mkdir(parents=True, exist_ok=True)

    for name, sf in DB_SIZES.items():
        db_path = DATABASES_DIR / f"{name}.db"
        if db_path.exists():
            print(f"  {name}.db already exists, skipping (delete to regenerate)")
            continue

        print(f"  Generating {name}.db (sf={sf})...", end="", flush=True)
        result = subprocess.run(
            [str(PACKDB_EXE), str(db_path), "-c",
             f"LOAD tpch; CALL dbgen(sf={sf});"],
            capture_output=True, text=True, timeout=300,
        )
        if result.returncode != 0:
            print(" FAILED")
            print(f"  Error: {result.stderr.strip()}", file=sys.stderr)
            sys.exit(1)
        print(f" OK ({db_path.stat().st_size / 1024 / 1024:.1f} MB)")

    print()
    print("Database sizes:")
    for name in DB_SIZES:
        db_path = DATABASES_DIR / f"{name}.db"
        if db_path.exists():
            size_mb = db_path.stat().st_size / 1024 / 1024
            print(f"  {name:>8}.db  {size_mb:>8.1f} MB")


if __name__ == "__main__":
    main()
