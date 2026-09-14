#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-g++}"
OUT="${TMPDIR:-/tmp}/vfn-transport-truth-test.$$"
trap 'rm -f "$OUT"' EXIT
"$CXX" -std=c++23 -O2 -Wall -Wextra -Werror \
    -I"$ROOT/src" \
    "$ROOT/tests/transport_truth_test.cpp" \
    "$ROOT/src/transport_truth.cpp" \
    -o "$OUT"
"$OUT"
