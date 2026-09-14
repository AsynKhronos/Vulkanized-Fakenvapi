#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
hooks="$root/src/audio_hooks.cpp"
timeline="$root/src/audio_timeline.cpp"
policy="$root/src/runtime_policy.h"
config="$root/src/config.cpp"
meson="$root/src/meson.build"

for needle in \
  'CoCreateInstance' \
  'IMMDevice::Activate' \
  'IAudioClient::Initialize' \
  'IAudioClient::GetBufferSize' \
  'IAudioClient::GetCurrentPadding' \
  'IAudioClient3::GetSharedModeEnginePeriod' \
  'IAudioClient3::GetCurrentSharedModeEnginePeriod' \
  'IAudioClient3::InitializeSharedAudioStream' \
  'IAudioClock::GetFrequency' \
  'IAudioClock::GetPosition'; do
  grep -q "$needle" "$hooks"
done

grep -q 'struct AudioPolicy' "$policy"
grep -q 'wasapi_observer' "$policy"
grep -q 'result.audio.wasapi_observer' "$config"
grep -q "'audio_timeline.cpp'" "$meson"
grep -q "'audio_hooks.cpp'" "$meson"

# Classic IAudioClient::Initialize remains a pure observer and must forward
# the exact application arguments. R4.5 may alter only PeriodInFrames on the
# application's own IAudioClient3::InitializeSharedAudioStream when explicitly enabled.
grep -q 'g_audio_initialize(self, mode, flags, buffer_duration, periodicity, format, session_guid)' "$hooks"
grep -q 'g_audio_initialize_shared(self, flags, effective_period_frames, format, session_guid)' "$hooks"
if grep -Eq 'IAudioRenderClient|ReleaseBuffer|SetClientProperties|resampl|src_process|soxr|Sleep\(|sleep_for|sleep_until' "$hooks" "$timeline"; then
  echo 'FAIL: AudioFlex contains forbidden render-buffer/pacing/resampling behavior' >&2
  exit 1
fi

# Padding and clock observation paths must stay fixed-state/atomic and silent
# per sample (no info/trace logging inside these hook bodies).
python - "$hooks" <<'PY'
import re, sys
s=open(sys.argv[1], encoding='utf-8').read()
for name in ('hkAudioGetCurrentPadding', 'hkAudioClockGetPosition'):
    m=re.search(r'HRESULT WINAPI '+name+r'\([^\)]*\) \{(.*?)\n\}', s, re.S)
    if not m:
        raise SystemExit(f'FAIL: missing {name}')
    body=m.group(1)
    if 'spdlog::' in body:
        raise SystemExit(f'FAIL: per-sample logging in {name}')
PY

echo 'AudioFlex WASAPI observer audit: PASS'
