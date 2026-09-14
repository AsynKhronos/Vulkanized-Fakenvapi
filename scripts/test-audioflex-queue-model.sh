#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

c++ -std=c++23 -O2 -Wall -Wextra -Werror \
  -I"$root/src" \
  "$root/tests/audio_queue_model_test.cpp" \
  -o "$tmp/audioflex-queue-model-test"

"$tmp/audioflex-queue-model-test"
