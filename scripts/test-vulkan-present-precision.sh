#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/vulkanized-fakenvapi-tests"
mkdir -p "$build_dir"
cxx="${CXX:-g++}"
"$cxx" -std=c++23 -O2 \
  -I"$root/src" -I"$root/external/vulkan/include" \
  "$root/tests/vulkan_present_precision_test.cpp" \
  "$root/src/vulkan_present_precision.cpp" \
  -o "$build_dir/vulkan_present_precision_test"
"$build_dir/vulkan_present_precision_test"
echo "PASS: Vulkan present precision tier unit test"
