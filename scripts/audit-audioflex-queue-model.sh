#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
hooks="$root/src/audio_hooks.cpp"
timeline="$root/src/audio_timeline.cpp"
model="$root/src/audio_queue_model.h"
policy="$root/src/runtime_policy.h"
config="$root/src/config.cpp"
ini="$root/fakenvapi.ini"

for needle in \
  'active_probe' \
  'queue_model' \
  'clock_probe_interval_ms'; do
  grep -q "$needle" "$policy"
  grep -q "$needle" "$config"
  grep -q "$needle" "$ini"
done

grep -q 'probe_audio_client_after_initialize' "$hooks"
grep -q 'GetSharedModeEnginePeriod' "$hooks"
grep -q 'GetCurrentSharedModeEnginePeriod' "$hooks"
grep -q 'IID_IAudioClock_' "$hooks"
grep -q 'g_co_task_mem_free(current_format)' "$hooks"
grep -q 'should_probe_clock' "$hooks"
grep -q 'clock_probe_interval_ms' "$hooks"
grep -q 'AudioFlex queue model active:' "$hooks"
grep -q 'endpoint_queue_only=true' "$hooks"
grep -q 'build_queue_estimate' "$model"
grep -q 'ShareMode::Shared' "$timeline"

# The active R4.4 probe may call getter-only methods, but it must never invoke
# any stream-mutating IAudioClient2/3 APIs or render-buffer methods itself.
python - "$hooks" <<'PY'
import re, sys
s=open(sys.argv[1], encoding='utf-8').read()
m=re.search(r'void probe_audio_client_after_initialize\((.*?)\n\}\n\nvoid maybe_probe_runtime_clock', s, re.S)
if not m:
    raise SystemExit('FAIL: missing active probe body')
body=m.group(1)
for forbidden in (
    'InitializeSharedAudioStream(', 'SetClientProperties(', 'SetEventHandle(',
    'IAudioRenderClient', 'ReleaseBuffer(', 'Start(', 'Stop(', 'Reset('):
    if forbidden in body:
        raise SystemExit(f'FAIL: active probe contains mutating call: {forbidden}')
for required in ('GetBufferSizeFn', 'GetStreamLatencyFn', 'GetCurrentPaddingFn',
                 'GetDevicePeriodFn', 'GetSharedModeEnginePeriodFn',
                 'GetCurrentSharedModeEnginePeriodFn'):
    if required not in body:
        raise SystemExit(f'FAIL: active probe missing getter: {required}')

m=re.search(r'HRESULT WINAPI hkAudioGetCurrentPadding\([^\)]*\) \{(.*?)\n\}', s, re.S)
if not m:
    raise SystemExit('FAIL: missing padding hook')
body=m.group(1)
if 'maybe_probe_runtime_clock' not in body or 'maybe_log_queue_model' not in body:
    raise SystemExit('FAIL: padding path does not feed throttled clock/queue model')
if 'spdlog::' in body:
    raise SystemExit('FAIL: per-padding direct logging reintroduced')
PY

# Queue latency is only labelled for shared-mode rendering padding, whose
# WASAPI semantics are queued frames waiting to play. Capture/exclusive stays
# outside the R4.4 playback-queue claim.
grep -q '!in.shared' "$model"
grep -q '!in.render' "$model"

# R4.4 still must not contain active period mutation/resampling/pacing.
if grep -Eq 'SetClientProperties|IAudioRenderClient|ReleaseBuffer|resampl|src_process|soxr|Sleep\(|sleep_for|sleep_until' "$hooks" "$timeline" "$model"; then
  echo 'FAIL: AudioFlex R4.4 contains mutation/pacing/resampling behavior' >&2
  exit 1
fi

echo 'AudioFlex R4.4 queue-model/active-probe audit: PASS'
