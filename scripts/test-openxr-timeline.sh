#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

c++ -std=c++23 -O2 -pthread -Wall -Wextra -Werror \
  -I"$root/src" \
  "$root/tests/openxr_timeline_test.cpp" \
  "$root/src/openxr_timeline.cpp" \
  -o "$tmp/openxr-timeline-test"

"$tmp/openxr-timeline-test"
