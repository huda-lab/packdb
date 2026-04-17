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
    "${VENV_DIR}/bin/pip" install --upgrade pip -q \
        --trusted-host pypi.org --trusted-host files.pythonhosted.org
    "${VENV_DIR}/bin/pip" install -r "${REQ_FILE}" -q \
        --trusted-host pypi.org --trusted-host files.pythonhosted.org
    echo "Virtualenv ready."
else
    # Ensure deps are up to date (fast no-op if already satisfied)
    "${VENV_DIR}/bin/pip" install -r "${REQ_FILE}" -q \
        --trusted-host pypi.org --trusted-host files.pythonhosted.org 2>/dev/null || true
fi

# ── Verify Gurobi oracle is usable ──────────────────────────────────────
# The oracle is Gurobi-only; catch missing install or bad license up front
# instead of surfacing it as a cryptic mid-test failure.
if ! "${VENV_DIR}/bin/python3" -c "import gurobipy; e = gurobipy.Env(empty=True); e.setParam('OutputFlag', 0); e.start()" 2>/dev/null; then
    echo "ERROR: gurobipy cannot start an environment." >&2
    echo "       Check that a valid Gurobi license is installed (grbprobe)," >&2
    echo "       or that GRB_LICENSE_FILE points to one." >&2
    exit 1
fi

# ── Handle --setup-only ──────────────────────────────────────────────────
if [[ "${1:-}" == "--setup-only" ]]; then
    echo "Setup complete. Virtualenv at: ${VENV_DIR}"
    exit 0
fi

# ── Run pytest ───────────────────────────────────────────────────────────
cd "${SCRIPT_DIR}"
exec "${VENV_DIR}/bin/python3" -m pytest tests/ -v "$@"
