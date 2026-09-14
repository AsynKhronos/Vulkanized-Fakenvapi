#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${CXX:-g++}"
OUT="${TMPDIR:-/tmp}/vf-frame-generation-cadence-router-test"
"$CXX" -std=c++23 -O2 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/tests/frame_generation_cadence_router_test.cpp" -o "$OUT"
"$OUT"
