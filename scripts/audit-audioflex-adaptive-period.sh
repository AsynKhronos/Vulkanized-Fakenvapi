#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOOK="$ROOT/src/audio_hooks.cpp"
POLICY="$ROOT/src/audio_period_policy.h"
CFG="$ROOT/src/runtime_policy.h"

grep -q 'adaptive_period = AutoBool::Auto' "$CFG"
grep -q 'action=recommend-only' "$HOOK"
grep -q 'no automatic retry on the same IAudioClient' "$HOOK"
# Exactly one product call site may initialize the app's shared stream.
[[ $(grep -c 'g_audio_initialize_shared(self, flags, effective_period_frames' "$HOOK") -eq 1 ]]
! grep -q 'adaptive-period fallback' "$HOOK"
grep -q 'GetSharedModeEnginePeriod' "$HOOK"
grep -q 'candidate < in.requested_frames' "$POLICY"

# R4.5 may negotiate only an application's existing IAudioClient3 init call.
! grep -q 'AudioRenderClient' "$POLICY"
! grep -q 'SetClientProperties' "$POLICY"
! grep -q 'Sleep(' "$POLICY"

echo "AudioFlex adaptive-period audit: PASS"
