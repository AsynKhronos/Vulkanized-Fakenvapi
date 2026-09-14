#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
hooks="$root/src/openxr_hooks.cpp"
abi="$root/src/openxr_abi.h"
util="$root/src/util.h"
timeline="$root/src/openxr_timeline.cpp"
policy="$root/src/runtime_policy.h"

for needle in \
  'xrCreateSession' \
  'xrConvertTimeToWin32PerformanceCounterKHR' \
  'XR_KHR_win32_convert_performance_counter_time' \
  'qpc_ticks_to_ns' \
  'clock_sync'; do
  grep -Rqs "$needle" "$hooks" "$abi" "$util" "$policy"
done

grep -q 'QueryPerformanceCounter' "$util"
grep -q 'QueryPerformanceFrequency' "$util"
# R4.2 intentionally removes wall-clock time from the common graphics/XR
# timestamp helper. The canonical domain must remain monotonic QPC.
if grep -q 'GetSystemTimePreciseAsFileTime' "$util"; then
  echo 'FAIL: canonical get_timestamp still uses wall clock' >&2
  exit 1
fi

# The conversion is performed for each relevant XrTime. Do not cache an
# assumed XrTime<->system-clock offset: OpenXR explicitly forbids that model.
grep -q 'xr_time_to_host_ns' "$hooks"
grep -q 'frame_state->predictedDisplayTime, xr.clock_sync, ClockSampleKind::Wait' "$hooks"
grep -q 'session, display_time, xr.clock_sync, ClockSampleKind::End' "$hooks"
if grep -Eqi 'xr.*clock.*offset|clock.*xr.*offset' "$hooks"; then
  echo 'FAIL: cached XR clock offset detected' >&2
  exit 1
fi

# Still observer-only: clock sync must not mutate XR timing or add a wait.
if grep -Eq 'predictedDisplayTime\s*=|predictedDisplayPeriod\s*=|displayTime\s*=' "$hooks"; then
  echo 'FAIL: R4.2 mutates OpenXR timing fields' >&2
  exit 1
fi
if grep -Eq 'Sleep\(|sleep_for|sleep_until|WaitForSingleObject|vkQueueSubmit|vkWaitForPresent' "$timeline"; then
  echo 'FAIL: R4.2 timeline contains an execution wait/submission path' >&2
  exit 1
fi

echo 'OpenXR canonical clock audit: PASS'
