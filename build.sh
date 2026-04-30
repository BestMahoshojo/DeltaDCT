#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
RUN_TESTS="${RUN_TESTS:-0}"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

if [[ "${RUN_TESTS}" == "1" ]]; then
	"${BUILD_DIR}/deltadct_phase3_tests"
	"${BUILD_DIR}/deltadct_phase45_tests"
fi

echo "Build complete (${BUILD_TYPE}): ${BUILD_DIR}/deltadct_tool"
echo "Python binding: ${BUILD_DIR}/pydeltadct*.so"
echo "Tip: RUN_TESTS=1 bash build.sh"
