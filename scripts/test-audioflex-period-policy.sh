#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
${CXX:-c++} -std=c++23 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/tests/audio_period_policy_test.cpp" -o "$TMP/audio_period_policy_test"
"$TMP/audio_period_policy_test"
echo "AudioFlex period policy test: PASS"
