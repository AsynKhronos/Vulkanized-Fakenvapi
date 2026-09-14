#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
audio="$root/src/audio_timeline.cpp"
audio_h="$root/src/audio_timeline.h"
hooks="$root/src/audio_hooks.cpp"
xr="$root/src/openxr_timeline.cpp"
xr_h="$root/src/openxr_timeline.h"
xr_hooks="$root/src/openxr_hooks.cpp"
low="$root/src/low_latency.cpp"

# AudioFlex Opt6: repetitive getters should use validated TLS caches; frequency
# is cached per initialized stream and queue diagnostics stop rebuilding after
# the one-shot report. Lifecycle snapshots reject concurrent reinitialization.
grep -q 'static thread_local std::uintptr_t cached_key' "$hooks"
grep -q 'queue_model_logged(client)' "$hooks"
grep -q 'clock_frequency(client)' "$hooks"
grep -q 'Initializing = 1u << 7' "$audio_h"
grep -q 'state.flags.load(std::memory_order_acquire) != flags' "$audio"

# XRFlex Opt6: session state and frame updates have explicit publication, clock
# bindings are generation-validated, and render correlation carries an epoch.
grep -q 'std::atomic<bool> ready' "$xr_h"
grep -q 'frame.revision.load(std::memory_order_acquire)' "$xr"
grep -q 'render_watermark_epoch' "$xr"
grep -q 'ClockDispatchSnapshot' "$xr_hooks"
grep -q 'binding->revision.load(std::memory_order_acquire)' "$xr_hooks"
grep -q 'render_epoch_watermark' "$low"

# Observer-only invariants remain non-negotiable.
if grep -Eq 'Sleep\(|sleep_for|sleep_until|WaitForSingleObject|vkQueueSubmit|vkQueueWaitIdle' "$xr"; then
  echo 'FAIL: XRFlex Opt6 introduced an execution wait/submission path' >&2
  exit 1
fi

echo 'AudioFlex/XRFlex Opt6 audit: PASS'
