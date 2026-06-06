#!/bin/bash
# Source this file to configure the TRACE-PV runtime environment:
#   source setup_env.sh

TRACE_PV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export LD_LIBRARY_PATH="${TRACE_PV_ROOT}/sqlite3/lib:${LD_LIBRARY_PATH}"

if [ -f "${TRACE_PV_ROOT}/venv/bin/activate" ]; then
    # shellcheck disable=SC1091
    source "${TRACE_PV_ROOT}/venv/bin/activate"
fi

export CUDA_PATH="${CUDA_PATH:-/usr/local/cuda}"
export PATH="${CUDA_PATH}/bin:${PATH}"

echo "TRACE-PV environment ready:"
echo "  Project root: ${TRACE_PV_ROOT}"
echo "  Python:       $(command -v python3)"
echo "  Simulator:    ${TRACE_PV_ROOT}/bin/trace_pv"
