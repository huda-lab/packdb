#!/usr/bin/env bash
clear
set -o pipefail

CONFIG_FILE="config.txt"
# Flag to track the current section
current_section=""
while IFS='=' read -r key value; do
  # Trim whitespace around 'key' and 'value'
  key=$(echo "$key" | xargs)
  value=$(echo "$value" | xargs)

  # Skip empty lines
  [[ -z "$key" ]] && continue

  # Detect section headers
  if [[ "$key" =~ ^\[.*\]$ ]]; then
    current_section="${key//[\[\]]/}" # Extract section name without brackets
    continue
  fi

  # Process keys based on the current section
  case "$current_section" in
    packdb)
      case "$key" in
        build_mode) BUILD_MODE=$value ;;
        db_file) DB_FILE=$value ;;
      esac
      ;;
  esac
done < "$CONFIG_FILE"

HOME=$(pwd)
python3 scripts/generate_grammar.py
python3 scripts/generate_flex.py

cd build/$BUILD_MODE

if ninja; then
  # Unittest
  # test/unittest [packdb]
  # Quicktest
  echo "./duckdb $DB_FILE < \"$(cat "$HOME/test/packdb/test.sql")\""
  ./duckdb "$DB_FILE" < "$HOME/test/packdb/test.sql"
else
  echo "Build failed. Skipping tests." >&2
  exit 1
fi