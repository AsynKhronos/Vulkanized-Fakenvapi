#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/vulkanized-fakenvapi-tests"
mkdir -p "$build_dir"
cxx="${CXX:-g++}"
"$cxx" -std=c++23 -O2 -Wall -Wextra -Werror \
  -I"$root/src" -I"$root/external/vulkan/include" \
  "$root/tests/vulkan_capabilities_test.cpp" \
  "$root/src/vulkan_capabilities.cpp" \
  -o "$build_dir/vulkan_capabilities_test"
"$build_dir/vulkan_capabilities_test"
echo "PASS: Vulkan capability engine unit test"
