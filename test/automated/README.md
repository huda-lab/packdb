# Automated Testing Framework for PackDB

## Usage

### Run Random Tests (Default)
```bash
cd /home/hqr9411/working_code/packdb
python3 test/automated/runner.py
```

### Test Your Own Query
```bash
# Test a specific SQL query with your data
python3 test/automated/runner.py --query my_query.sql --data my_data.csv

# With a custom test name
python3 test/automated/runner.py --query query.sql --data data.csv --name my_test
```

Results are saved in `test/automated/results/{timestamp}/` with timestamped subdirectories for each run.

## Output Structure

Each test creates its own directory with 4 key files:

```
results/{timestamp}/{test_name}/
├── query.sql             # SQL query for PackDB
├── model.mps             # MPS optimization problem for HiGHS
├── highs_solution.csv    # Solution from HiGHS (row-by-row x values + objective)
└── packdb_solution.csv   # Solution from PackDB
```

This makes it easy to compare outputs side-by-side.

## What This Framework Does

1. **Generates** random optimization problems (knapsack, equality, minimize, etc.)
2. **Solves** them using HiGHS solver (trusted oracle)
3. **Solves** them using PackDB's DECIDE clause
4. **Compares** the results to verify correctness

## Query Types Tested

The framework tests various combinations of:

- **Variable Types**: BINARY (`x IS BINARY`)
- **Objectives**: MAXIMIZE, MINIMIZE
- **Constraints**: `<=`, `>=`, `=`
- **Complexity**: Single and multiple constraints

## Adding New Tests

To add a new test type, add a static method to `DataGenerator`:

```python
@staticmethod
def generate_my_problem(num_items=10, seed=None):
    prob = ProblemDefinition()
    prob.num_rows = num_items
    prob.columns = ['id', 'col1', 'col2']
    prob.variable_type = "BINARY"
    prob.objective_sense = "MAXIMIZE"
    prob.objective_expr = "x * col1"
    
    # Add data and constraints...
    
    return prob
```

Then call it in `main()`:
```python
problem = DataGenerator.generate_my_problem(seed=999)
runner.run_test(problem, "my_test_name", work_dir)
```

## Current Status (3/5 tests passing)

✅ **Working:**
- Knapsack problems (MAXIMIZE with `<=`)
- Multiple `<=` constraints

❌ **Failing (DuckDB crashes):**
- Equality constraints (`=`)
- MINIMIZE objective

## Next Steps

1. **Fix the bugs**: Investigate and repair the assertion failures for equality constraints and MINIMIZE
2. **Expand coverage**: Add INTEGER variable tests, BETWEEN constraints, IN constraints
3. **Improve verification**: Extract actual solution vectors from PackDB to compute exact objective values
4. **Add edge cases**: Empty tables, infeasible problems, unbounded problems
