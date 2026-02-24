#!/usr/bin/env bash
# run_tests.sh — Run DECIDE pytest suite inside a managed virtualenv.
#
# Usage:
#   ./run_tests.sh                    # Run all tests
#   ./run_tests.sh -m var_boolean     # Run only boolean variable tests
#   ./run_tests.sh -k test_q01        # Run tests matching pattern
#   ./run_tests.sh --setup-only       # Just create/update the venv
#
# The virtualenv is created at test/decide/.venv/ on first run.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${SCRIPT_DIR}/.venv"
REQ_FILE="${SCRIPT_DIR}/requirements.txt"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Create or reuse virtualenv ───────────────────────────────────────────
if [ ! -d "${VENV_DIR}" ]; then
    echo "Creating virtualenv at ${VENV_DIR} ..."
    python3 -m venv "${VENV_DIR}"
    echo "Installing dependencies ..."
    "${VENV_DIR}/bin/pip" install --upgrade pip -q
    "${VENV_DIR}/bin/pip" install -r "${REQ_FILE}" -q
    # Install packdb in editable mode
    if [ -d "${REPO_ROOT}/tools/pythonpkg" ]; then
        echo "Installing packdb (editable) ..."
        "${VENV_DIR}/bin/pip" install -e "${REPO_ROOT}/tools/pythonpkg" -q
    fi
    echo "Virtualenv ready."
else
    # Ensure deps are up to date (fast no-op if already satisfied)
    "${VENV_DIR}/bin/pip" install -r "${REQ_FILE}" -q 2>/dev/null || true
fi

# ── Handle --setup-only ──────────────────────────────────────────────────
if [[ "${1:-}" == "--setup-only" ]]; then
    echo "Setup complete. Virtualenv at: ${VENV_DIR}"
    exit 0
fi

# ── Run pytest ───────────────────────────────────────────────────────────
cd "${SCRIPT_DIR}"
exec "${VENV_DIR}/bin/python3" -m pytest tests/ -v "$@"
