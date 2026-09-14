#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/vulkanized-fakenvapi-tests"
mkdir -p "$build_dir"
cxx="${CXX:-g++}"
"$cxx" -std=c++23 -O2 -I"$root/src" \
  "$root/tests/wait_policy_test.cpp" \
  -o "$build_dir/wait_policy_test"
"$build_dir/wait_policy_test"
echo "PASS: adaptive wait policy unit test"
