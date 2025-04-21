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
    tpch)
      case "$key" in
        scale_factor) SCALE_FACTOR=$value ;;
      esac
      ;;
  esac
done < "$CONFIG_FILE"

BUILD_GENERATOR=""
BUILD_COMMAND=""
HOME=$PWD

if command -v ninja >/dev/null 2>&1; then
  echo "Ninja executable found. Using Ninja build system."
  BUILD_GENERATOR="Ninja"
  BUILD_COMMAND="ninja"
else
  echo "Ninja executable not found. Using Make build system."
  BUILD_GENERATOR="Unix Makefiles"
  BUILD_COMMAND="make -j$(nproc)"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -f CMakeCache.txt ]; then
    echo "Existing CMakeCache.txt found. Removing for potentially fresh configuration."
    rm CMakeCache.txt
fi

cmake -G "$BUILD_GENERATOR" ..
$BUILD_COMMAND

SQL_SCRIPT=$(mktemp)
cat << EOF > "$SQL_SCRIPT"
LOAD tpch;
CALL dbgen(sf = $SCALE_FACTOR);
EOF
./duckdb "$DB_FILE" < "$SQL_SCRIPT"
if [ $? -ne 0 ]; then
    echo "Error: Failed to set up TPC-H."
    rm -f "$SQL_SCRIPT"
    exit 1
fi
rm -f "$SQL_SCRIPT"
