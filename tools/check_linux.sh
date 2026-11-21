#!/usr/bin/env bash
# Data-free native Linux regression suite. Run explicitly with bash.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $(uname -s) != Linux ]]; then
    echo 'Run this script on Linux (or inside WSL), not a Windows shell.' >&2
    exit 1
fi
build=${1:-"$root/build/linux-platform"}
cmake -S "$root/runtime" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
    -DMKW_BUILD_PRODUCTS=OFF -DMKW_PLATFORM_TESTS_ONLY=ON -DMKW_BUILD_PSQ_TESTS=ON
cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
ctest --test-dir "$build" --output-on-failure
