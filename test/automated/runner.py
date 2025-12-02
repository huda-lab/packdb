#!/usr/bin/env python3
"""
Automated Testing Framework for PackDB - Refactored

This framework tests PackDB's DECIDE clause by:
1. Auto-discovering all queries in test/automated/queries/
2. Using the real packdb.db database
3. Comparing HiGHS solver results with PackDB results in matching table formats
"""

import subprocess
import os
import sys
import csv
import glob
from pathlib import Path
from datetime import datetime
import re


def run_highs_solver(mps_file, solution_file):
    """Run HiGHS solver and return status + solution"""
    highs_bin = Path(__file__).parent.parent.parent / "build" / "release" / "bin" / "highs"
    
    result = subprocess.run(
        [str(highs_bin), mps_file, f"--solution_file={solution_file}", "--time_limit=60.0"],
        capture_output=True,
        text=True
    )
    
    output = result.stdout + result.stderr
    
    # Parse status and objective
    status = "UNKNOWN"
    objective = None
    
    for line in output.split('\n'):
        if 'Status' in line and 'Optimal' in line:
            status = 'OPTIMAL'
        elif 'Infeasible' in line and 'Status' in line:
            status = 'INFEASIBLE'
        elif 'Primal bound' in line:
            parts = line.split()
            for i, part in enumerate(parts):
                if part == 'bound' and i + 1 < len(parts):
                    try:
                        objective = float(parts[i + 1])
                        break
                    except ValueError:
                        pass
    
    # Parse solution file
    solution = {}
    if os.path.exists(solution_file):
        with open(solution_file, 'r') as f:
            lines = f.readlines()
            in_columns = False
            for line in lines:
                line = line.strip()
                if 'Columns' in line or 'Column' in line:
                    in_columns = True
                    continue
                if 'Rows' in line: # Stop if we hit Rows section
                    in_columns = False
                    continue
                    
                if in_columns and line and not line.startswith('#'):
                    parts = line.split()
                    if len(parts) >= 2:
                        try:
                            var_name = parts[0]
                            var_value = float(parts[1])
                            # If integer, convert to int? Or keep as float?
                            # PackDB output might be float. Let's keep as float or round if close to int.
                            if abs(var_value - round(var_value)) < 1e-6:
                                var_value = int(round(var_value))
                            solution[var_name] = var_value
                        except (ValueError, IndexError):
                            continue
    
    return status, objective, solution


def run_packdb_query(query_file, db_file):
    """Run the query using PackDB (DuckDB)"""
    # Assuming read_config is defined elsewhere or will be added.
    # For now, we'll define a placeholder for read_config to make the code syntactically correct.
    # In a real scenario, this would come from a configuration module.
    def read_config():
        duckdb_bin = Path(__file__).parent.parent.parent / "build" / "release" / "duckdb"
        return duckdb_bin, None # Placeholder for other config value

    duckdb_bin, _ = read_config()
    
    # Use -csv flag to ensure CSV output
    cmd = [str(duckdb_bin), str(db_file), '-csv']
    
    # Read query content
    with open(query_file, 'r') as f:
        query_sql = f.read()
        
    print(f"ℹ Running PackDB with: {' '.join(cmd)}")
    result = subprocess.run(cmd, input=query_sql, capture_output=True, text=True)
    
    return result.stdout, result.stderr, result.returncode


def extract_table_name_from_query(query_content):
    """Extract table name from SQL query"""
    # Look for FROM clause
    for line in query_content.upper().split('\n'):
        if 'FROM' in line and 'SELECT' not in line:
            parts = line.split('FROM')[1].strip().split()
            if parts:
                return parts[0].strip()
    return "table"


def extract_where_clause(query_content):
    """Extract WHERE clause from SQL query"""
    # Normalize
    upper_query = query_content.upper()
    
    if 'WHERE' not in upper_query:
        return ""
        
    # Find start of WHERE
    start_idx = upper_query.find('WHERE') + 5
    
    # Find end of WHERE (DECIDE, GROUP BY, ORDER BY, LIMIT, ;, or end of string)
    end_markers = ['DECIDE', 'GROUP BY', 'ORDER BY', 'LIMIT', ';']
    end_idx = len(query_content)
    
    for marker in end_markers:
        idx = upper_query.find(marker, start_idx)
        if idx != -1 and idx < end_idx:
            end_idx = idx
            
    return query_content[start_idx:end_idx].strip()


def strip_comments(sql):
    """Strip SQL comments"""
    import re
    # Remove multi-line comments
    sql = re.sub(r'/\*[\s\S]*?\*/', '', sql)
    # Remove single-line comments
    lines = []
    for line in sql.split('\n'):
        line = line.split('--')[0]
        if line.strip():
            lines.append(line)
    return '\n'.join(lines)


def query_to_mps(query_file, db_file, mps_file):
    """Convert SQL query to MPS format by querying the real database"""
    duckdb_bin = Path(__file__).parent.parent.parent / "build" / "release" / "duckdb"
    
    # Read the query
    with open(query_file, 'r') as f:
        raw_content = f.read()
        
    # Strip comments for parsing
    sql_content = strip_comments(raw_content)
    
    # Extract table name
    table_name = extract_table_name_from_query(sql_content)
    
    # Extract WHERE clause
    where_clause = extract_where_clause(sql_content)
    
    # Parse DECIDE clause components
    # Look for DECIDE x, y, ...
    decide_vars = []
    if 'DECIDE' in sql_content:
        decide_part = sql_content.split('DECIDE')[1]
        # It ends at SUCH THAT or MAXIMIZE or MINIMIZE or ;
        end_markers = ['SUCH THAT', 'MAXIMIZE', 'MINIMIZE', ';']
        end_idx = len(decide_part)
        for marker in end_markers:
            idx = decide_part.find(marker)
            if idx != -1:
                end_idx = min(end_idx, idx)
        
        vars_str = decide_part[:end_idx].strip()
        decide_vars = [v.strip().lower() for v in vars_str.split(',')]
    
    if not decide_vars:
        decide_vars = ['x'] # Default fallback
        
    variable_type = "INTEGER"
    if 'IS BINARY' in sql_content:
        variable_type = "BINARY"
    
    # Get objective sense and expression
    if 'MAXIMIZE' in sql_content:
        obj_sense = "MAXIMIZE"
        obj_part = sql_content.split('MAXIMIZE')[1].strip().rstrip(';')
    elif 'MINIMIZE' in sql_content:
        obj_sense = "MINIMIZE"
        obj_part = sql_content.split('MINIMIZE')[1].strip().rstrip(';')
    else:
        print("ERROR: No objective found")
        return False
    
    # Extract objective expression
    if 'SUM(' in obj_part:
        obj_expr = obj_part.split('SUM(')[1].split(')')[0].strip().lower()
    else:
        # Fallback if no SUM (unlikely for PackDB)
        obj_expr = "0" 
    
    # Parse constraints and bounds
    constraints = []
    bounds = {}  # {var_name: {'lower': 0, 'upper': None}}
    
    # Initialize bounds for all decide vars
    # Default bounds: 0 to +inf for both Integer and Binary (Binary is 0-1)
    for var in decide_vars:
        bounds[var] = {'lower': 0.0, 'upper': float('inf')}
        if variable_type == "BINARY":
            bounds[var]['upper'] = 1.0
    
    if 'SUCH THAT' in sql_content:
        constraint_part = sql_content.split('SUCH THAT')[1]
        if 'MAXIMIZE' in constraint_part:
            constraint_part = constraint_part.split('MAXIMIZE')[0]
        elif 'MINIMIZE' in constraint_part:
            constraint_part = constraint_part.split('MINIMIZE')[0]
            
        # Normalize spaces
        constraint_part = ' '.join(constraint_part.split())
        
        # Split by ' AND ' (case insensitive)
        import re
        parts = re.split(r'\s+AND\s+', constraint_part, flags=re.IGNORECASE)
        
        for part in parts:
            part = part.strip()
            if not part:
                continue
                
            # Type declarations
            if 'IS BINARY' in part.upper():
                continue 
            if 'IS INTEGER' in part.upper():
                continue
                
            # SUM constraints
            if 'SUM(' in part.upper():
                # Parse SUM(...) <= RHS
                try:
                    match = re.search(r'(<=|>=|=|<|>)', part)
                    if not match:
                        continue
                        
                    operator = match.group(1)
                    lhs, rhs_str = part.split(operator)
                    
                    if 'SUM(' in lhs.upper():
                        # Use case-insensitive split or just lowercase everything
                        # We want the content inside SUM(...)
                        # lhs might be "SUM(x * price)" or "sum(x * price)"
                        # Regex is safer
                        sum_match = re.search(r'SUM\((.*)\)', lhs, re.IGNORECASE)
                        if sum_match:
                            sum_expr = sum_match.group(1).strip().lower()
                        else:
                            continue
                    else:
                        continue
                        
                    rhs = float(rhs_str.strip())
                    
                    sense = 'EQ'
                    if '<' in operator:
                        sense = 'LE'
                        if operator == '<' and variable_type == "INTEGER":
                            rhs -= 1
                    elif '>' in operator:
                        sense = 'GE'
                        if operator == '>' and variable_type == "INTEGER":
                            rhs += 1
                                
                    constraints.append({
                        'expr': sum_expr,
                        'type': sense,
                        'rhs': rhs
                    })
                except (IndexError, ValueError):
                    print(f"Warning: Failed to parse constraint: {part}")
                    continue
            
            # Variable bounds (x > 2, y < 10)
            else:
                match = re.match(r'([a-zA-Z0-9_]+)\s*(<=|>=|=|<|>)\s*(-?[\d\.]+)', part)
                if match:
                    var = match.group(1).lower()
                    op = match.group(2)
                    val = float(match.group(3))
                    
                    if var in bounds:
                        if '<' in op:
                            limit = val
                            if op == '<' and variable_type == "INTEGER":
                                limit -= 1
                            bounds[var]['upper'] = min(bounds[var]['upper'], limit)
                        elif '>' in op:
                            limit = val
                            if op == '>' and variable_type == "INTEGER":
                                limit += 1
                            bounds[var]['lower'] = max(bounds[var]['lower'], limit)
                        elif '=' in op:
                            bounds[var]['lower'] = val
                            bounds[var]['upper'] = val
    
    # Execute the query file to set up tables (CREATE TABLE + INSERT)
    # We'll execute everything up to the SELECT statement
    
    setup_sql = []
    has_ddl = False
    ddl_keywords = ['CREATE', 'INSERT', 'DROP', 'COPY']
    
    for statement in sql_content.split(';'):
        statement = statement.strip()
        if not statement:
            continue
        if statement.upper().startswith('SELECT'):
            break
        setup_sql.append(statement)
        upper_stmt = statement.upper()
        if any(k in upper_stmt for k in ddl_keywords):
            has_ddl = True
    
    if has_ddl:
        drop_table_cmd = f"DROP TABLE IF EXISTS {table_name};"
        subprocess.run([str(duckdb_bin), str(db_file)], input=drop_table_cmd, capture_output=True, text=True)
        setup_commands = ';\n'.join(setup_sql) + ';'
        result = subprocess.run([str(duckdb_bin), str(db_file)], input=setup_commands, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"ERROR setting up tables: {result.stderr}")
            return False
    else:
        print(f"ℹ No DDL found in query file, using existing table '{table_name}'")
    
    # Query the database to get actual data
    data_query = f"SELECT * FROM {table_name}"
    if where_clause:
        data_query += f" WHERE {where_clause}"
    data_query += ";"
    
    print(f"ℹ Fetching data with: {data_query}")
    
    result = subprocess.run([str(duckdb_bin), str(db_file), '-csv'], input=data_query, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"ERROR querying database: {result.stderr}")
        return False
    
    lines = result.stdout.strip().split('\n')
    if len(lines) < 2:
        print("ERROR: No data returned from database")
        return False
    
    reader = csv.DictReader(lines)
    data = []
    columns = None
    for row in reader:
        # Normalize keys to lowercase
        row = {k.lower(): v for k, v in row.items()}
        
        if columns is None:
            columns = list(row.keys())
        int_row = {}
        for k, v in row.items():
            try:
                int_row[k] = int(v)
            except (ValueError, TypeError):
                int_row[k] = v
        data.append(int_row)
        
    # Check for infeasibility in bounds for BINARY variables
    if variable_type == "BINARY":
        for var, b in bounds.items():
            if b['lower'] > 1 or b['upper'] < 0:
                return False, "INFEASIBLE", data, columns, obj_sense, decide_vars
            if b['lower'] > b['upper']:
                return False, "INFEASIBLE", data, columns, obj_sense, decide_vars
    elif variable_type == "INTEGER":
         for var, b in bounds.items():
            if b['lower'] > b['upper']:
                return False, "INFEASIBLE", data, columns, obj_sense, decide_vars

    # Adjust RHS for constant terms in constraints
    for constraint in constraints:
        constant_sum = 0.0
        for row in data:
            constant_sum += eval_constant_part(constraint['expr'], row, decide_vars)
        
        constraint['rhs'] -= constant_sum
    
    # Write MPS file
    with open(mps_file, 'w') as f:
        f.write("NAME          PACKDB_TEST\n")
        f.write("ROWS\n")
        f.write(" N  OBJ\n")
        for i, constraint in enumerate(constraints):
            sense_char = {'LE': 'L', 'GE': 'G', 'EQ': 'E'}[constraint['type']]
            f.write(f" {sense_char}  C{i}\n")
        
        f.write("COLUMNS\n")
        
        # If INTEGER, start INTORG marker
        if variable_type == "INTEGER":
             f.write("    MARK0000  'MARKER'                 'INTORG'\n")
             
        for row_idx, row in enumerate(data):
            # For each decision variable
            for var_name in decide_vars:
                col_name = f"R{row_idx}_{var_name}"
                
                # Objective coefficient
                obj_coef = eval_linear_coef(obj_expr, row, var_name)
                if obj_sense == "MAXIMIZE":
                    obj_coef = -obj_coef
                
                if obj_coef != 0:
                    f.write(f"    {col_name:<20}  OBJ       {obj_coef}\n")
                
                # Constraint coefficients
                for c_idx, constraint in enumerate(constraints):
                    coef = eval_linear_coef(constraint['expr'], row, var_name)
                    if coef != 0:
                        f.write(f"    {col_name:<20}  C{c_idx:<8}  {coef}\n")
        
        # If INTEGER, end INTEND marker
        if variable_type == "INTEGER":
             f.write("    MARK0001  'MARKER'                 'INTEND'\n")
        
        f.write("RHS\n")
        for c_idx, constraint in enumerate(constraints):
            f.write(f"    RHS1      C{c_idx:<8}  {constraint['rhs']}\n")
        
        f.write("BOUNDS\n")
        for row_idx, row in enumerate(data):
            for var_name in decide_vars:
                col_name = f"R{row_idx}_{var_name}"
                lower = bounds[var_name]['lower']
                upper = bounds[var_name]['upper']
                
                if variable_type == "BINARY":
                    f.write(f" BV BOUND1    {col_name}\n")
                    # Write bounds if they conflict with or tighten 0-1
                    if lower > 0:
                        f.write(f" LI BOUND1    {col_name:<20}  {lower}\n")
                    if upper < 1:
                        f.write(f" UI BOUND1    {col_name:<20}  {upper}\n")
                else:
                    # Integer variables (default 0 to inf)
                    # MPS default is 0 to +inf if not specified.
                    # If lower != 0, specify LI.
                    # If upper != inf, specify UI.
                    if lower != 0:
                        if lower == -float('inf'):
                             f.write(f" FR BOUND1    {col_name}\n") # Free variable
                        else:
                             f.write(f" LI BOUND1    {col_name:<20}  {lower}\n")
                    
                    if upper != float('inf'):
                        f.write(f" UI BOUND1    {col_name:<20}  {upper}\n")
                    else:
                        # If unbounded integer, some solvers default to binary (0-1).
                        # We must specify a large upper bound.
                        f.write(f" UI BOUND1    {col_name:<20}  2147483647\n")
        
        f.write("ENDATA\n")
    
    return True, "OPTIMAL", data, columns, obj_sense, decide_vars


def split_expr(expr):
    """Split expression by '+' respecting parentheses"""
    terms = []
    current_term = []
    paren_level = 0
    
    # Normalize: replace - with + - (but careful about parens)
    # Actually, let's just iterate chars.
    # If we see '-', and we are not in parens, and previous char was not operator...
    # This is hard.
    # Simpler: Use the previous normalization `replace('-', '+-')` but ONLY if - is not inside parens?
    # No, `(a-b)` should remain `(a-b)`.
    # `a - b` -> `a + -b`.
    
    # Let's just iterate and split by '+' at level 0.
    # And handle '-' by treating it as '+' followed by '-' term?
    # e.g. "a - b" -> "a", "-b".
    
    # Pre-process: replace " - " with " + -".
    # But "a-b" (no spaces) -> "a+-b".
    # "(a-b)" -> "(a-b)".
    
    # Let's iterate.
    i = 0
    n = len(expr)
    start = 0
    
    while i < n:
        char = expr[i]
        if char == '(':
            paren_level += 1
        elif char == ')':
            paren_level -= 1
        elif char == '+' and paren_level == 0:
            terms.append(expr[start:i].strip())
            start = i + 1
        elif char == '-' and paren_level == 0:
            # If it's the start of expression or after another operator, it's unary minus.
            # e.g. "-a". start=0. i=0.
            # e.g. "a * -b". Not possible if we split by +.
            # e.g. "a - b".
            # If we split at '-', we need to include '-' in the next term.
            # And the previous term ends here.
            
            # Check if it's binary minus.
            # It is binary if previous char is not operator.
            # But we are only splitting by +/-(level 0).
            
            # If i > start, then we have a term before this '-'.
            # e.g. "a-b". start=0. i=1. expr[0]='a'.
            # We append 'a'.
            # Then we set start = i. So next term starts with '-'.
            if i > start:
                terms.append(expr[start:i].strip())
                start = i
            # If i == start (e.g. "-a"), we continue.
            
        i += 1
    
    terms.append(expr[start:].strip())
    return [t for t in terms if t]


def eval_constant_part(expr, row, decide_vars):
    """Evaluate constant part of expression (terms without decision vars)"""
    # Remove spaces? No, split_expr handles stripping.
    # But we should remove spaces inside terms for eval?
    # eval() handles spaces.
    
    terms = split_expr(expr)
    total_const = 0.0
    
    for term in terms:
        # Check if term contains ANY decision var
        # Tokenize by * to find vars?
        # But term might be "(a+b)*x".
        # We need to check if var is present as a token.
        # Simple heuristic: check if var string is in term?
        # "tax" contains "x". False positive!
        # We need to tokenize properly.
        # Regex `\bvar\b`?
        
        has_var = False
        for var in decide_vars:
            # Check for whole word match
            if re.search(r'\b' + re.escape(var) + r'\b', term):
                has_var = True
                break
        
        if not has_var:
            # Evaluate this constant term
            py_term = term.replace('^', '**')
            
            # Replace column names with values
            cols = sorted(row.keys(), key=len, reverse=True)
            for col in cols:
                # Use regex to replace whole word column name
                # e.g. "tax" should not replace "l_tax" (if l_tax is in cols)
                # But we sort by length desc, so l_tax is replaced first.
                # But "tax" might replace "tax_rate"?
                # Yes. Regex replacement is safer.
                
                if col in py_term:
                    # Escape col for regex
                    pattern = r'\b' + re.escape(col) + r'\b'
                    val = str(row[col])
                    py_term = re.sub(pattern, val, py_term)
            
            try:
                val = eval(py_term)
                total_const += float(val)
            except Exception as e:
                print(f"DEBUG: Failed to eval constant term: {term}. Error: {e}")
                pass
                
    return total_const


def eval_linear_coef(expr, row, var_name):
    """Evaluate coefficient of var_name in linear expression"""
    terms = split_expr(expr)
    total_coef = 0.0
    
    for term in terms:
        # Check if term contains var_name
        if not re.search(r'\b' + re.escape(var_name) + r'\b', term):
            continue
            
        # We assume the term is linear: coeff * var
        # e.g. "3 * x * price"
        # or "x"
        # or "-x"
        # or "(price + 1) * x"
        
        # We want to evaluate everything EXCEPT var_name.
        # We can replace var_name with 1.0 and eval?
        # If term is "x * x", replacing with 1.0 gives 1.0. But it's non-linear.
        # We should check linearity.
        # Count occurrences?
        matches = re.findall(r'\b' + re.escape(var_name) + r'\b', term)
        if len(matches) != 1:
            continue # Non-linear or missing
            
        # Replace var_name with 1.0 and eval
        py_term = term.replace('^', '**')
        
        # Replace var_name with 1.0
        py_term = re.sub(r'\b' + re.escape(var_name) + r'\b', '1.0', py_term)
        
        # Replace columns
        cols = sorted(row.keys(), key=len, reverse=True)
        for col in cols:
            if col in py_term:
                pattern = r'\b' + re.escape(col) + r'\b'
                val = str(row[col])
                py_term = re.sub(pattern, val, py_term)
                
        try:
            val = eval(py_term)
            total_coef += float(val)
        except Exception:
            pass
            
    return total_coef


def eval_expr(expr, row):
    """Evaluate simple expression like 'x * column'"""
    if '*' in expr:
        parts = [p.strip() for p in expr.split('*')]
        if parts[0] == 'x' and len(parts) == 2:
            col_name = parts[1]
            return row.get(col_name, 0)
    return 1


def write_highs_solution_csv(data, columns, solution, objective, obj_sense, output_file, decide_vars=['x']):
    """Write HiGHS solution in table format"""
    with open(output_file, 'w', newline='') as f:
        writer = csv.writer(f)
        
        # Header: decide_vars + all original columns
        header = decide_vars + columns
        writer.writerow(header)
        
        for row_idx, row in enumerate(data):
            # Get values for all decision variables
            var_values = []
            for var in decide_vars:
                col_name = f"R{row_idx}_{var}"
                val = solution.get(col_name, 0)
                var_values.append(val)
            
            writer.writerow(var_values + list(row.values()))
        
        # Blank line then objective
        writer.writerow([])
        actual_obj = objective if obj_sense == "MINIMIZE" else -objective if objective else None
        writer.writerow(['objective', actual_obj])


def run_test(query_file, db_file, output_dir):
    """Run a single test"""
    test_name = Path(query_file).stem
    print(f"\\n{'='*60}")
    print(f"Testing: {test_name}")
    print(f"{'='*60}")
    
    # Create output directory
    test_dir = Path(output_dir) / test_name
    test_dir.mkdir(parents=True, exist_ok=True)
    
    # 1. Copy query.sql
    query_out = test_dir / "query.sql"
    with open(query_file, 'r') as f:
        query_content = f.read()
    with open(query_out, 'w') as f:
        f.write(query_content)
    print(f"✓ Copied query.sql")
    
    # 3. Generate MPS file from query and data
    mps_file = test_dir / 'model.mps'
    result = query_to_mps(query_file, db_file, mps_file)
    if not result:
        print("ERROR: Failed to generate MPS file")
        return False
        
    success, status, data, columns, obj_sense, decide_vars = result
    
    highs_solution_csv = test_dir / 'highs_solution.csv'
    highs_status_file = test_dir / 'highs_status.txt'
    
    if not success:
        if status == "INFEASIBLE":
            print("✓ Detected INFEASIBLE problem during parsing")
            objective = 0.0
            solution = {}
            with open(highs_status_file, 'w') as f:
                f.write("Status: INFEASIBLE\nObjective: 0.0\n")
            # Create empty solution file
            with open(highs_solution_csv, 'w') as f:
                pass
        else:
            print("ERROR: Failed to generate MPS file")
            return False
    else:
        print(f"✓ Generated model.mps ({len(data)} rows)")
        
        # 4. Run HiGHS solver
        status, objective, solution = run_highs_solver(mps_file, highs_solution_csv)
        
        # Write solution to CSV (overwrite with formatted output)
        write_highs_solution_csv(data, columns, solution, objective, obj_sense, highs_solution_csv, decide_vars)
    
    # 5. Run PackDB
    packdb_solution_csv = test_dir / 'packdb_solution.csv'
    packdb_status_file = test_dir / 'packdb_status.txt'
    
    print(f"✓ HiGHS: {status}, Objective: {objective}")
    
    # Write status file
    with open(highs_status_file, 'w') as f:
        f.write(f"Status: {status}\n")
        f.write(f"Objective: {objective}\n")
    
    # 4. Run PackDB
    packdb_out, packdb_err, returncode = run_packdb_query(query_file, db_file)
    
    if returncode == 0:
        # Parse output to extract CSV
        # print(f"DEBUG: PackDB Output (first 10 lines):\n{chr(10).join(packdb_out.splitlines()[:10])}")
        csv_content = parse_packdb_output(packdb_out, known_columns=columns)
        if csv_content:
            with open(packdb_solution_csv, 'w') as f:
                f.write(csv_content)
        else:
            print("ERROR: Failed to parse CSV from PackDB output")
        with open(packdb_status_file, 'w') as f:
            f.write("Status: SUCCESS\n")
        print(f"✓ PackDB: SUCCESS")
        
        # 5. Compare Results
        match, reason = compare_results(highs_solution_csv, packdb_solution_csv, sort_cols=['l_orderkey', 'l_linenumber'])
        if match:
            print(f"✓ Comparison: PASS")
            return True
        else:
            print(f"✗ Comparison: FAIL ({reason})")
            return False
            
    else:
        print(f"✗ PackDB: FAILED")
        print(packdb_err)
        with open(packdb_status_file, 'w') as f:
            f.write(f"Status: FAILED\nError: {packdb_err}\n")
        return False
    
    return True


def read_config(repo_root):
    """Read config.txt to find db_file"""
    config_file = repo_root / "config.txt"
    
    # Defaults
    build_mode = "release"
    db_filename = "packdb.db"
    
    if config_file.exists():
        with open(config_file, 'r') as f:
            section = None
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                if line.startswith('[') and line.endswith(']'):
                    section = line[1:-1]
                    continue
                
                if section == 'packdb' and '=' in line:
                    key, value = [p.strip() for p in line.split('=', 1)]
                    if key == 'db_file':
                        db_filename = value
                    elif key == 'build_mode':
                        build_mode = value
    
    # Construct path: build/<mode>/<db_file>
    # Check if the file exists there, otherwise fall back to root
    db_path = repo_root / "build" / build_mode / db_filename
    
    if not db_path.exists():
        # Fallback to root
        db_path = repo_root / db_filename
        
    return db_path


def parse_packdb_output(output, known_columns=None):
    """Extract CSV content from noisy PackDB output"""
    lines = output.split('\n')
    csv_lines = []
    
    # Heuristic: Skip lines that look like logs or banners
    skip_keywords = [
        "running highs", "copyright", "licence", "debug:", "info:", 
        "warning:", "error:", "time:", "rows:", "threads:", "cols:", "bounds",
        "writing the model", "presolve", "solving", "model status", "objective value",
        "primal bound", "dual bound", "gap", "solution", "iterations", "nodes",
        "mip", "nonzeros", "integer variables", "matrix", "coefficient ranges"
    ]
    
    for line in lines:
        line = line.strip()
        if not line:
            continue
            
        lower_line = line.lower()
        if any(k in lower_line for k in skip_keywords):
            continue
            
        # If we have known columns, require at least one to be present in the header
        # This helps distinguish the CSV header from random logs that might contain commas
        if known_columns and not csv_lines:
            # We are looking for the header
            # Check if any known column is in the line (case insensitive)
            line_lower = line.lower()
            found_col = False
            for col in known_columns:
                if col.lower() in line_lower:
                    found_col = True
                    break
            if not found_col:
                continue
        
        if ',' not in line:
            if ' ' in line:
                continue
        
        csv_lines.append(line)
        
    return '\n'.join(csv_lines)


def compare_results(highs_csv, packdb_csv, sort_cols=None):
    """Compare HiGHS and PackDB results"""
    try:
        with open(highs_csv, 'r') as f:
            highs_rows = list(csv.reader(f))
        
        with open(packdb_csv, 'r') as f:
            packdb_rows = list(csv.reader(f))
            
        if not highs_rows or not packdb_rows:
            return False, "Empty result file"
            
        # Normalize headers (lowercase, strip)
        h_header = [c.strip().lower() for c in highs_rows[0]]
        p_header = [c.strip().lower() for c in packdb_rows[0]]
        
        # Find index of decision variables (assume 'x' or 'x_value' or similar)
        # Actually, the user wants to compare "decision variables".
        # In HiGHS output we write 'x_value'. 
        # In PackDB output, it depends on the SELECT clause.
        # We need to match columns by name.
        
        common_cols = set(h_header) & set(p_header)
        if not common_cols:
            return False, f"No common columns. HiGHS: {h_header}, PackDB: {p_header}"
            
        # Sort both by all common columns to ensure alignment
        # But wait, floating point comparison might be needed.
        
        # Filter rows to data only
        h_data = highs_rows[1:]
        p_data = packdb_rows[1:]
        
        # Remove empty lines or summary lines (like "objective")
        h_data = [r for r in h_data if r and len(r) == len(h_header) and r[0] != 'objective']
        p_data = [r for r in p_data if r and len(r) == len(p_header)]
        
        # Create dictionaries for comparison: {row_key: row_dict}
        # We need a primary key. If none, we can just sort and compare line by line.
        # Let's try sorting by all columns.
        
        def row_to_dict(header, row):
            return {k: v for k, v in zip(header, row)}
            
        h_dicts = [row_to_dict(h_header, r) for r in h_data]
        p_dicts = [row_to_dict(p_header, r) for r in p_data]
        
        if sort_cols:
            # Check if cols exist
            missing = [c for c in sort_cols if c not in h_header or c not in p_header]
            if missing:
                print(f"Warning: Sort cols {missing} not found. Falling back to full sort.")
            else:
                def sort_key(d):
                    return tuple(d[k] for k in sort_cols)
                
                h_dicts.sort(key=sort_key)
                p_dicts.sort(key=sort_key)
                
        else:
            # Sort by all keys
            def sort_key(d):
                return tuple(sorted(d.items()))
                
            h_dicts.sort(key=sort_key)
            p_dicts.sort(key=sort_key)
        
        if len(h_dicts) != len(p_dicts):
            return False, f"Row count mismatch: HiGHS={len(h_dicts)}, PackDB={len(p_dicts)}"
            
        for i, (h, p) in enumerate(zip(h_dicts, p_dicts)):
            for col in common_cols:
                h_val = h[col]
                p_val = p[col]
                
                # Loose equality check
                def normalize(v):
                    v_str = str(v).lower().strip()
                    if v_str in ['true', '1', '1.0']: return 1
                    if v_str in ['false', '0', '0.0']: return 0
                    try:
                        f = float(v)
                        if f.is_integer(): return int(f)
                        return f
                    except ValueError:
                        return v_str
                
                if normalize(h_val) != normalize(p_val):
                    # Try float comparison with tolerance
                    try:
                        f_h = float(h_val)
                        f_p = float(p_val)
                        if abs(f_h - f_p) > 1e-4:
                            return False, f"Mismatch in row {i}, col '{col}': {h_val} != {p_val}"
                    except ValueError:
                         return False, f"Mismatch in row {i}, col '{col}': '{h_val}' != '{p_val}'"
        
        return True, "Match"
        
    except Exception as e:
        return False, f"Comparison error: {e}"


def main():
    """Main entry point"""
    import argparse
    
    parser = argparse.ArgumentParser(description='PackDB Test Runner')
    parser.add_argument('query_file', nargs='?', help='Path to specific SQL query file to test')
    args = parser.parse_args()

    # Paths
    repo_root = Path(__file__).parent.parent.parent
    db_file = read_config(repo_root)
    queries_dir = Path(__file__).parent / "queries"
    results_base = Path(__file__).parent / "results"
    
    # Check database exists
    if not db_file.exists():
        print(f"ERROR: Database not found: {db_file}")
        print("Please run ./run.sh first to create the database")
        return 1
        
    # Determine queries to run
    if args.query_file:
        query_path = Path(args.query_file)
        if not query_path.exists():
            print(f"ERROR: Query file not found: {query_path}")
            return 1
        query_files = [str(query_path.resolve())]
    else:
        # Find all query files
        query_files = sorted(glob.glob(str(queries_dir / "*.sql")))
        if not query_files:
            print(f"ERROR: No query files found in {queries_dir}")
            return 1
    
    print(f"Found {len(query_files)} queries to test")
    print(f"Database: {db_file}")
    
    # Create timestamped output directory
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = results_base / timestamp
    output_dir.mkdir(parents=True, exist_ok=True)
    
    print(f"Output directory: {output_dir}\\n")
    
    # Run all tests
    passed = 0
    failed = 0
    
    for query_file in query_files:
        if run_test(query_file, db_file, output_dir):
            passed += 1
        else:
            failed += 1
    
    # Summary
    print(f"\\n{'='*60}")
    print(f"Test Summary: {passed} passed, {failed} failed")
    print(f"Results saved to: {output_dir}")
    print(f"{'='*60}")
    
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
