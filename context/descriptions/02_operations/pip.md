# Python Package (pip install)

## 1. Overview
PackDB is distributed as a Python package named `packdb`. The package builds the entire C++ codebase (DuckDB core + PackDB extensions + HiGHS solver) from source into a single compiled `.so` extension module, then wraps it with Python bindings via pybind11.

Key files:
- `tools/pythonpkg/setup.py` — Main build logic
- `tools/pythonpkg/pyproject.toml` — PEP 517 build config, CI/cibuildwheel settings
- `scripts/package_build.py` — Source/include gathering for the amalgamation build path
- `scripts/amalgamation.py` — Lists all core DuckDB sources and includes

## 2. Build Paths
`setup.py` has two distinct build paths depending on whether `DUCKDB_BINARY_DIR` is set:

### 2.1 From-Source Build (default, used by `pip install`)
When `DUCKDB_BINARY_DIR` is **not** set (the normal `pip install .` path):
1. `setup.py` detects that `scripts/amalgamation.py` exists (meaning we're in the repo, not a sdist).
2. Calls `package_build.build_package()` which:
   - Uses `amalgamation.list_sources()` to gather all core DuckDB `.cpp` files.
   - Uses `package_build.third_party_sources()` to gather third-party source directories (including `third_party/highs`).
   - Uses `package_build.third_party_includes()` to gather all include paths.
   - Copies everything into a `duckdb_build/` staging directory inside `tools/pythonpkg/`.
   - Generates unity build files where CMakeLists.txt uses `add_library_unity`.
3. All gathered sources are compiled into a single `packdb._packdb` extension module (the `.so`).

### 2.2 Pre-Built Binary Path (CMake `BUILD_PYTHON=1`)
When `DUCKDB_BINARY_DIR` **is** set:
1. Only the Python binding sources (`tools/pythonpkg/src/`) are compiled.
2. Links against the pre-built `duckdb_static` library and extension libraries found in the binary dir.
3. Third-party includes (including SymbolicC++, HiGHS) are added via `package_build.third_party_includes()`.

## 3. HiGHS Integration in pip
HiGHS is compiled directly into the wheel — there is no external dependency or system library required.

- **Sources**: `package_build.third_party_sources()` includes `third_party/highs`, which causes all HiGHS `.cpp`, `.cc`, and `.c` files to be gathered recursively.
- **Includes**: `package_build.third_party_includes()` lists all HiGHS subdirectories (lines 42–62 in `package_build.py`): root, `highs/`, `extern/`, `extern/filereaderlp`, `extern/pdqsort`, and every HiGHS module directory (`interfaces`, `io`, `ipm`, `ipm/ipx`, `ipm/basiclu`, `lp_data`, `mip`, `model`, `parallel`, `pdlp`, `pdlp/cupdlp`, `presolve`, `qpsolver`, `simplex`, `test_kkt`, `util`).
- **CMake build**: `third_party/highs/CMakeLists.txt` builds `duckdb_highs` as a static library. `src/CMakeLists.txt` links both `duckdb` and `duckdb_static` against `duckdb_highs`.

## 4. Package Structure
The installed `packdb` package contains:
```
packdb/
├── __init__.py              # Re-exports everything from ._packdb
├── _packdb.cpython-*.so     # The compiled C++ extension (DuckDB + PackDB + HiGHS)
├── functional/              # UDF type helpers
├── typing/                  # Type system wrappers
├── value/                   # Value type wrappers
├── query_graph/             # Query graph utilities
├── experimental/spark/      # Spark SQL compatibility layer
└── ...
```

The compiled `.so` is named `_packdb` (as `lib_name + '._packdb'` in setup.py, where `lib_name = 'packdb'`). All Python modules import from `packdb._packdb` (the `.so`) using `from ._packdb import ...`.

## 5. How to Build Locally

### Prerequisites
- Python 3.8+
- `pybind11`, `setuptools`, `setuptools_scm` (installed automatically by PEP 517)

### Using a virtual environment
```bash
cd tools/pythonpkg
python3 -m venv venv
source venv/bin/activate
pip install .
```

A venv already exists at `tools/pythonpkg/venv/` (gitignored). To use it:
```bash
cd tools/pythonpkg
./venv/bin/pip install .
```

### Editable install (recommended for development)
Use `pip install -e .` to install in editable/development mode. This builds the `_packdb.cpython-*.so` directly into the `packdb/` source directory, so the local directory *is* the installed package — no shadowing issues:
```bash
cd tools/pythonpkg
source venv/bin/activate
pip install -e .
```

### Important: shadowing with non-editable installs
If you use `pip install .` (non-editable), **do not** run Python from within `tools/pythonpkg/`. The local `packdb/` source directory will shadow the installed package, causing `ModuleNotFoundError: No module named 'packdb._packdb'` because the source directory lacks the compiled `.so`. Run from any other directory:
```bash
cd /tmp
/path/to/venv/bin/python -c "import packdb; print(packdb.connect().sql('SELECT 42').fetchall())"
```

### Using CMake (alternative)
```bash
BUILD_PYTHON=1 make
# or
mkdir build && cd build
cmake .. -DBUILD_PYTHON=1
make
```

## 6. Key Internals

### Source Gathering (`package_build.py`)
- `third_party_includes()` — Returns all third-party include paths needed for compilation. Any new third-party dependency must be added here for pip builds to work.
- `third_party_sources()` — Returns directories containing third-party `.cpp/.c/.cc` source files. These are recursively scanned for source files during the amalgamation.
- `build_package()` — Orchestrates the full amalgamation: copies sources, generates unity builds, patches version info.

### Unity Builds
The amalgamation path detects `add_library_unity` in CMakeLists.txt files and generates unity build `.cpp` files that `#include` multiple source files. This speeds up compilation significantly.

### Version Detection
Version is derived from `git describe --tags --long` via `setuptools_scm` (configured in `pyproject.toml` with `root = "../.."`). The version format is `0.1.devN` where N is the number of commits since the last tag.

## 7. CI / Wheel Building
The GitHub Actions workflow (`.github/workflows/python-wheels.yml`) uses `cibuildwheel` to build cross-platform wheels. It is triggered by `workflow_dispatch` (manual, requires a version input) or on GitHub release publish.

- **Platforms**: ubuntu-22.04 (Linux x86_64), macos-14 (macOS x86_64 + arm64), windows-2019 (AMD64).
- **Python versions**: cp38 through cp312.
- **Smoke test**: After building, runs `python -c "import packdb; print(packdb.__version__)"`.
- **Artifacts**: Wheels are uploaded as GitHub Actions artifacts (7-day retention). The sdist is built separately on Linux.
- **PyPI publish**: Only runs on release events or when `publish_to_pypi` is set in manual dispatch. Uses trusted publishing (`id-token: write`).

`pyproject.toml` also configures `cibuildwheel` defaults (used when building locally with `cibuildwheel`):
- Runs `pytest` test suite after building.
- musllinux, i686, and aarch64 run a reduced "fast" test suite.

### Prerequisites for CI
- **HiGHS must be committed**: `third_party/highs/` must be tracked in git (not just present locally). The workflow checks out the repo with `submodules: recursive`, but HiGHS is not a submodule — it must be committed directly.
- **Version tags**: `setuptools_scm` derives version from git tags. Without tags, version will be `0.1.devN`. The workflow sets `OVERRIDE_GIT_DESCRIBE` but this only affects CMake builds, not the pip/setuptools_scm path.
