#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FILE="$ROOT/src/low_latency.cpp"

grep -q 'selected_input.compare_exchange_weak' "$FILE"
if grep -q 'selected_input.store(desired' "$FILE"; then
    echo 'FAIL: stale unconditional selected_input.store() arbitration remains' >&2
    exit 1
fi
grep -q 'Re-evaluate against that new incumbent' "$FILE"
grep -q 'applied_sleep_mode_\.store(0' "$FILE"

echo 'PASS: concurrent input selection uses CAS arbitration'

# Disabled frontends may still be observed but must not win arbitration.
grep -q 'frontend_selection_mask()' "$ROOT/src/low_latency.cpp"
grep -q 'kSleepModeEnabled' "$ROOT/src/low_latency.cpp"
grep -q 'eligible_mask' "$ROOT/src/input_arbiter.cpp"

# A healthy equal-quality incumbent must remain sticky; priority is only an
# initial tie-breaker, never a reason to bounce an active source.
if grep -q 'best_quality == current_quality && best_priority < current_priority' "$ROOT/src/input_arbiter.cpp"; then
    echo 'FAIL: equal-quality incumbent can still be displaced by priority' >&2
    exit 1
fi
echo 'PASS: disabled frontends excluded and equal-quality incumbent is sticky'
