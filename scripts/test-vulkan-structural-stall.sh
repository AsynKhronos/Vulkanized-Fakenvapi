#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
"$CXX" -std=c++23 -O2 -Wall -Wextra -Werror \
  tests/vulkan_structural_stall_test.cpp src/vulkan_structural_stall.cpp \
  -o "$OUT"
"$OUT"
