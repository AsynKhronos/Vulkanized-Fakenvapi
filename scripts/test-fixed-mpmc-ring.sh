#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${CXX:-c++}"
OUT="${TMPDIR:-/tmp}/fakenvapi-fixed-mpmc-ring-test"
"$CXX" -std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic \
    -I"$ROOT/src" \
    "$ROOT/tests/fixed_mpmc_ring_test.cpp" \
    -o "$OUT"
"$OUT"
rm -f "$OUT"
