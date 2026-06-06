#!/bin/bash
# Wrapper script to run nrsdb.py with the virtual environment

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VENV_PYTHON="${PROJECT_ROOT}/venv/bin/python3"

if [ ! -x "$VENV_PYTHON" ]; then
    echo "Error: Project virtual environment not found at ${PROJECT_ROOT}/venv"
    echo "Please run: python3 -m venv ${PROJECT_ROOT}/venv && ${PROJECT_ROOT}/venv/bin/python3 -m pip install pandas requests"
    exit 1
fi

# Run the script with the virtual environment's Python
exec "$VENV_PYTHON" "${SCRIPT_DIR}/nrsdb.py" "$@"

