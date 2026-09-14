#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

c++ -std=c++23 -O2 -pthread -Wall -Wextra -Werror \
  -I"$root/src" \
  "$root/tests/audio_timeline_test.cpp" \
  "$root/src/audio_timeline.cpp" \
  -o "$tmp/audioflex-timeline-test"

"$tmp/audioflex-timeline-test"
