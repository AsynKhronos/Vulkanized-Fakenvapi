#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
# The vendored header defines its std::max compatibility macro on MinGW.
"$CXX" -D__MINGW64__ -std=c++23 -O2 -Wall -Wextra -Werror \
  tests/latencyflex_structural_compensation_test.cpp -o "$OUT"
"$OUT"
