---
name: test-decide
description: Run PackDB differential tests comparing DECIDE output vs HiGHS oracle
---

Run the PackDB automated differential test suite.

## Steps

1. Ensure build is up to date: check if `build/release/packdb` exists. If not, run `make` first.
2. Run: `python test/automated/runner.py`
3. Report results: which queries passed, which failed, any objective value mismatches.
4. If a test fails, examine the MPS file and PackDB output to diagnose whether the issue is in formulation or execution.
