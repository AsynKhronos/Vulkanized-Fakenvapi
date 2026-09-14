#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
util="$root/src/util.h"
policy="$root/src/wait_policy.h"

for f in "$util" "$policy"; do test -f "$f"; done

grep -q 'static thread_local ThreadWaitableTimer timer' "$util"
grep -q 'CREATE_WAITABLE_TIMER_HIGH_RESOLUTION' "$util"
grep -q 'CreateWaitableTimerExW(nullptr, nullptr, 0' "$util"
grep -q 'return waitdetail::spin_until(deadline);' "$util"
grep -q 'cpu_relax();' "$util"
grep -q "kMaxSpinTailNs = 500'000" "$policy"

if grep -q "busywait_threshold = 2'000'000" "$util"; then
  echo 'FAIL: legacy fixed 2 ms spin threshold still present' >&2
  exit 1
fi
if grep -q 'static HANDLE timer = CreateWaitableTimer' "$util"; then
  echo 'FAIL: waitable timer is still shared globally between pacing threads' >&2
  exit 1
fi

echo 'PASS: low-jitter wait behavior / per-thread timer audit'
