#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

c++ -std=c++23 -O2 -pthread \
  -I"$root/src" \
  "$root/tests/hybrid_fusion_test.cpp" \
  "$root/src/hybrid_fusion.cpp" \
  "$root/src/input_arbiter.cpp" \
  "$root/src/runtime_policy.cpp" \
  -o "$tmp/hybrid-fusion-test"

"$tmp/hybrid-fusion-test"
