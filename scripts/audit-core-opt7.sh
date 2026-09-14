#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

require() {
  local pattern=$1 file=$2
  grep -Fq "$pattern" "$file" || { echo "FAIL missing '$pattern' in ${file#$ROOT/}" >&2; exit 1; }
}
forbid() {
  local pattern=$1 file=$2
  if grep -Fq "$pattern" "$file"; then
    echo "FAIL forbidden '$pattern' in ${file#$ROOT/}" >&2
    exit 1
  fi
}

# Input arbitration: native-frame activity, not callback density, drives age.
require 'if (frame_advanced) {' "$ROOT/src/input_arbiter.cpp"
require 'observation_sequence_.fetch_add(1' "$ROOT/src/input_arbiter.cpp"
require 'const bool newly_observed_marker' "$ROOT/src/input_arbiter.cpp"
forbid 'score + 1' "$ROOT/src/input_arbiter.cpp"
require 'Freeze the observation watermark once' "$ROOT/src/input_arbiter.cpp"

# FG detection: fixed O(1) storage, hysteresis and bounded producer migration.
require 'class FrameGenerationCadenceDetector' "$ROOT/src/frame_generation_cadence.h"
require 'kComparisonWindow = 11' "$ROOT/src/frame_generation_cadence.h"
require 'std::uint16_t repeat_bits_' "$ROOT/src/frame_generation_cadence.h"
require 'kProgressPublishStride = 2' "$ROOT/src/frame_generation_cadence_router.h"
require 'repeat_count_ >= kEnableRepeatThreshold' "$ROOT/src/frame_generation_cadence.h"
require 'repeat_count_ == 0' "$ROOT/src/frame_generation_cadence.h"
require 'class FrameGenerationCadenceRouter' "$ROOT/src/frame_generation_cadence_router.h"
require 'kTakeoverLagFrames = 3' "$ROOT/src/frame_generation_cadence_router.h"
require 'owner_.compare_exchange_strong' "$ROOT/src/frame_generation_cadence_router.h"
require 'if (changed)' "$ROOT/src/low_latency_d3d.cpp"
require 'update_effective_fg_state();' "$ROOT/src/low_latency_d3d.cpp"

# LatencyFleX context state must be per-instance and externally synchronized.
require 'std::mutex operation_mutex_' "$ROOT/src/low_latency_tech/ll_latencyflex.h"
require 'std::mutex lifetime_mutex_' "$ROOT/src/low_latency_tech/ll_latencyflex.h"
require 'std::uint32_t timeout_events_' "$ROOT/src/low_latency_tech/ll_latencyflex.h"
require 'needs_reset_.exchange(false' "$ROOT/src/low_latency_tech/ll_latencyflex.cpp"
forbid 'static uint64_t timeout_events' "$ROOT/src/low_latency_tech/ll_latencyflex.cpp"

"$ROOT/scripts/test-frame-generation-cadence.sh" >/dev/null
"$ROOT/scripts/test-frame-generation-cadence-router.sh" >/dev/null
"$ROOT/scripts/test-policy-engine.sh" >/dev/null

echo 'PASS: Opt7 core arbitration / FG cadence / LatencyFleX invariants'
