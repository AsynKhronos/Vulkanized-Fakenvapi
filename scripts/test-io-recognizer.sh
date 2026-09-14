#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

c++ -std=c++23 -O2 -Wall -Wextra -Werror \
    -I"$ROOT/src" \
    "$ROOT/tests/io_recognizer_test.cpp" \
    "$ROOT/src/runtime_policy.cpp" \
    "$ROOT/src/policy_engine.cpp" \
    "$ROOT/src/output_recognizer.cpp" \
    "$ROOT/src/input_arbiter.cpp" \
    -o "$OUT/io-recognizer-test"

"$OUT/io-recognizer-test"
