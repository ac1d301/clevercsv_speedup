#!/usr/bin/env bash
# Build the v2 type_detector pybind11 extension.
#
# Usage (Colab):
#     bash src/build_v2.sh
#
# Output: ${OUT_DIR}/type_detector<ext_suffix>  (default OUT_DIR=/content)
#
# Requires: g++, python with pybind11 installed (pip install pybind11).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SCRIPT_DIR}/type_detector_v2.cpp"
OUT_DIR="${OUT_DIR:-/content}"
PYTHON_BIN="${PYTHON_BIN:-python}"

if [[ ! -f "${SRC}" ]]; then
    echo "ERROR: source not found at ${SRC}" >&2
    exit 1
fi

if ! command -v g++ >/dev/null 2>&1; then
    echo "ERROR: g++ not found on PATH" >&2
    exit 1
fi

if ! "${PYTHON_BIN}" -c "import pybind11" >/dev/null 2>&1; then
    echo "ERROR: pybind11 not importable by ${PYTHON_BIN}. Install with: pip install pybind11" >&2
    exit 1
fi

PYBIND_INCLUDES="$(${PYTHON_BIN} -m pybind11 --includes)"
EXT_SUFFIX="$(${PYTHON_BIN} -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))')"

mkdir -p "${OUT_DIR}"
OUT="${OUT_DIR}/type_detector${EXT_SUFFIX}"

echo "Source : ${SRC}"
echo "Output : ${OUT}"
echo "Python : $(${PYTHON_BIN} --version 2>&1)"
echo "Flags  : -O3 -march=native -shared -fPIC -std=c++17 -Wall"
echo

# shellcheck disable=SC2086
g++ -O3 -march=native -shared -fPIC -std=c++17 \
    -Wall -Wno-multichar \
    ${PYBIND_INCLUDES} \
    "${SRC}" \
    -o "${OUT}"

echo
echo "Built ${OUT}"
ls -lh "${OUT}"
