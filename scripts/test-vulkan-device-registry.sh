#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/vulkanized-fakenvapi-tests"
mkdir -p "$build_dir"

cxx="${CXX:-g++}"

"$cxx" \
  -std=c++23 \
  -O2 \
  -pthread \
  -I"$root/src" \
  -I"$root/external/vulkan/include" \
  -I"$root/external/spdlog/include" \
  "$root/tests/vulkan_device_registry_test.cpp" \
  "$root/src/vulkan_device_registry.cpp" \
  -o "$build_dir/vulkan_device_registry_test"

"$build_dir/vulkan_device_registry_test"
echo "PASS: Vulkan device registry unit test"
