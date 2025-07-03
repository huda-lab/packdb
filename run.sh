#!/usr/bin/env bash
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
        build_dir) BUILD_DIR=$value ;;
        db_file) DB_FILE=$value ;;
      esac
      ;;
  esac
done < "$CONFIG_FILE"

HOME=$(pwd)
python3 scripts/generate_grammar.py
python3 scripts/generate_flex.py

if command -v ninja >/dev/null 2>&1; then
  echo "Ninja executable found. Using Ninja build system."
  BUILD_GENERATOR="Ninja"
  BUILD_COMMAND="ninja"
else
  echo "Ninja executable not found. Using Make build system."
  BUILD_GENERATOR="Unix Makefiles"
  BUILD_COMMAND="make -j$(nproc)"
fi

cd "$BUILD_DIR"
$BUILD_COMMAND

# Quicktest
./duckdb "$DB_FILE" < "$HOME/test/packdb/test.sql"
# Unittest
test/unittest [packdb]