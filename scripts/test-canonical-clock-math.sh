#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
c++ -std=c++23 -O2 -Wall -Wextra -Werror \
  -I"$root/src" "$root/tests/canonical_clock_math_test.cpp" \
  -o "$tmp/canonical-clock-math-test"
"$tmp/canonical-clock-math-test"
