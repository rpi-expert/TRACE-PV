#!/bin/bash
# Wrapper script to run nrsdb.py with the virtual environment

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_PYTHON="${SCRIPT_DIR}/venv/bin/python3"

if [ ! -f "$VENV_PYTHON" ]; then
    echo "Error: Virtual environment not found at ${SCRIPT_DIR}/venv"
    echo "Please run: python3 -m venv venv && ./venv/bin/pip install pandas requests"
    exit 1
fi

# Run the script with the virtual environment's Python
exec "$VENV_PYTHON" "${SCRIPT_DIR}/nrsdb.py" "$@"

