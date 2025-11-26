# Testing Framework

This document describes the automated testing framework for PackDB's DECIDE clause.

## Overview

The testing framework is designed to verify the correctness of the DECIDE clause by comparing PackDB's results against a Python oracle that uses the HiGHS solver.

## Location

The test scripts are located in `test/automated/`.

## Usage

To run a single test case:

```bash
python3 test/automated/runner.py "path/to/query.sql"
```

To run all tests in `test/automated/queries/`:

```bash
python3 test/automated/runner.py
```

## How it Works

The `runner.py` script performs the following steps for each test query:

1.  **Parse & Solve (Oracle):**
    *   Parses the SQL query to formulate an Integer Linear Program (ILP).
    *   Generates an MPS file (`model.mps`).
    *   Solves the ILP using the HiGHS solver.
    *   Saves the solution to `highs_solution.csv`.
    *   Saves the solver status to `highs_status.txt`.

2.  **Execute (PackDB):**
    *   Sends the same query to the PackDB pipeline.
    *   Saves the result to `packdb_solution.csv`.
    *   Saves the execution status to `packdb_status.txt`.

3.  **Compare:**
    *   Compares the decision variables in `highs_solution.csv` and `packdb_solution.csv`.
    *   Reports PASS if the decision vectors are identical, otherwise FAIL.

## Output

For each test, the following files are generated in a timestamped directory (or specific test directory):

*   `query.sql`: The executed query.
*   `model.mps`: The ILP formulation in MPS format.
*   `highs_solution.csv`: The oracle's solution.
*   `packdb_solution.csv`: PackDB's solution.
*   `highs_status.txt`: HiGHS solver status and objective.
*   `packdb_status.txt`: PackDB execution status.

## Database

The tests use the `packdb.db` database defined in `config.txt` and initialized by `run.sh`. Ensure you have run `./run.sh` at least once to set up the database.
