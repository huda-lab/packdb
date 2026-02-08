        1 # Plan: Vendor HiGHS into `third_party/highs/`
        2
        3 ## Context
        4
        5 HiGHS is currently fetched via CMake `FetchContent` from GitHub at build time. This means the Py
          thon/pip build (which uses DuckDB's amalgamation system scanning `third_party/`) can't find HiGH
          S sources or headers. Every other dependency (re2, mbedtls, fmt, zstd, etc.) is vendored locally
           in `third_party/` — HiGHS should follow the same pattern.
        6
        7 ## Goal
        8
        9 After this change:
       10 - `make` / `make debug` works exactly as before
       11 - `pip install .` from `tools/pythonpkg/` works standalone (no prior CMake build needed)
       12 - No network fetch at build time
       13
       14 ## Steps
       15
       16 ### 1. Copy HiGHS v1.11.0 sources into `third_party/highs/`
       17
       18 From `build/release/_deps/highs-src/`, copy only what's needed:
       19
       20 ```
       21 third_party/highs/
       22   HConfig.h          # Static config (see step 2)
       23   CMakeLists.txt     # New (see step 3)
       24   LICENSE            # From highs-src/LICENSE.txt
       25   highs/             # All source/header subdirs from highs-src/highs/
       26     Highs.h
       27     interfaces/, io/, ipm/, lp_data/, mip/, model/,
       28     parallel/, pdlp/, presolve/, qpsolver/, simplex/,
       29     test_kkt/, util/
       30   extern/            # External deps
       31     filereaderlp/    # reader.cpp + headers
       32     pdqsort/         # pdqsort.h (header-only)
       33 ```
       34
       35 **Exclude:** `app/`, `tests/`, `check/`, `docs/`, `examples/`, `nuget/`, `.git/`, `.github/`, `h
          ighs/highspy/`, `highs/highs_bindings.cpp`, `extern/catch.hpp`, `extern/zstr/`, `highs/HConfig.h
          .*.in`, `highs/meson.build`, `cmake/`
       36
       37 ### 2. Create static `third_party/highs/HConfig.h`
       38
       39 Use platform-conditional defines so it works cross-platform without CMake configure:
       40
       41 ```c
       42 #ifndef HCONFIG_H_
       43 #define HCONFIG_H_
       44 #define FAST_BUILD
       45 #define CUPDLP_CPU
       46 #define CMAKE_BUILD_TYPE "Release"
       47 #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
       48 #define HIGHS_HAVE_MM_PAUSE
       49 #endif
       50 #if defined(__GNUC__) || defined(__clang__)
       51 #define HIGHS_HAVE_BUILTIN_CLZ
       52 #endif
       53 #if defined(_MSC_VER)
       54 #define HIGHS_HAVE_BITSCAN_REVERSE
       55 #endif
       56 #define HIGHS_GITHASH "364c83a51"
       57 #define HIGHS_VERSION_MAJOR 1
       58 #define HIGHS_VERSION_MINOR 11
       59 #define HIGHS_VERSION_PATCH 0
       60 #endif
       61 ```
       62
       63 Key: `ZLIB_FOUND` is NOT defined (avoids zlib dependency; PackDB never reads compressed MPS file
          s).
       64
       65 ### 3. Create `third_party/highs/CMakeLists.txt`
       66
       67 - List all source files explicitly (from `cmake/sources.cmake`): 28 BasicLU `.c`, 8 cuPDLP `.c`,
           31 IPX `.cc`, ~57 core `.cpp` files
       68 - Target name: `duckdb_highs` (follows DuckDB convention)
       69 - Set include directories via `target_include_directories`
       70 - Call `disable_target_warnings(duckdb_highs)` to suppress third-party warnings
       71 - Install with `DUCKDB_EXPORT_SET`
       72
       73 ### 4. Modify root `CMakeLists.txt`
       74
       75 - **Remove** lines 1301-1311 (FetchContent block)
       76 - **Add** at line ~664 (alongside other third_party includes):
       77   ```cmake
       78   include_directories(third_party/highs)
       79   include_directories(third_party/highs/highs)
       80   ```
       81
       82 ### 5. Modify `third_party/CMakeLists.txt`
       83
       84 Add `add_subdirectory(highs)` inside `if(NOT AMALGAMATION_BUILD)` block (line 16).
       85
       86 ### 6. Update `highs::highs` → `duckdb_highs` (4 places)
       87
       88 | File | Change |
       89 |------|--------|
       90 | `src/CMakeLists.txt:136` | `target_link_libraries(duckdb duckdb_highs)` |
       91 | `src/CMakeLists.txt:142` | `target_link_libraries(duckdb_static duckdb_highs)` |
       92 | `src/execution/operator/decide/CMakeLists.txt:2` | `target_link_libraries(duckdb_operator_deci
          de duckdb_highs)` |
       93 | `src/packdb/naive/CMakeLists.txt:5` | `target_link_libraries(duckdb_packdb_naive duckdb_highs)
          ` |
       94
       95 ### 7. Update `scripts/package_build.py`
       96
       97 In `third_party_includes()`, add:
       98 ```python
       99 includes += [os.path.join('third_party', 'highs')]
      100 includes += [os.path.join('third_party', 'highs', 'highs')]
      101 includes += [os.path.join('third_party', 'highs', 'extern')]
      102 includes += [os.path.join('third_party', 'highs', 'extern', 'filereaderlp')]
      103 includes += [os.path.join('third_party', 'highs', 'extern', 'pdqsort')]
      104 # + all highs subdirs (ipm, ipm/ipx, ipm/basiclu, lp_data, mip, etc.)
      105 ```
      106
      107 In `third_party_sources()`, add:
      108 ```python
      109 sources += [os.path.join('third_party', 'highs')]
      110 ```
      111
      112 (The amalgamation recursively scans this for `.cpp/.cc/.c` files.)
      113
      114 ### 8. Revert previous workarounds in `setup.py`
      115
      116 - Revert `DUCKDB_PYTHON_LIB_NAME` back to `lib_name` (the `duckdb` hack was only needed for bina
          ry-dir approach)
      117 - Remove the `highs_src_dir` include hack from the binary-dir path
      118
      119 Actually — keep `DUCKDB_PYTHON_LIB_NAME` as `'duckdb'` since that was needed to fix the `PyInit_
          duckdb` mismatch regardless of HiGHS.
      120
      121 ## Verification
      122
      123 1. **CMake release build:** `rm -rf build/release && make` — verify `packdb` binary works
      124 2. **Run DECIDE test:** `./build/release/packdb -c "CREATE TABLE t(w INT, v INT); INSERT INTO t
          VALUES (3,4),(4,5),(2,3); SELECT *, x FROM t DECIDE x SUCH THAT SUM(x*w)<=5 AND x IS BINARY MAXI
          MIZE SUM(x*v);"`
      125 3. **Pip build:** `cd tools/pythonpkg && pip install .` (no env vars needed)
      126 4. **Python test:** `python test_packdb.py`
      127 5. **Automated tests:** `python test/automated/runner.py`