#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"

require() {
    local needle="$1" file="$2"
    grep -Fq "$needle" "$file" || { echo "FAIL missing '$needle' in $file" >&2; exit 1; }
}

require "startup_begin_sequence_" "$root/src/hybrid_fusion.h"
require "lock_state_" "$root/src/hybrid_fusion.h"
require "startup_observations" "$root/src/runtime_policy.h"
require "publish_pacing_observation" "$root/src/hybrid_fusion.cpp"
require "maybe_lock_startup" "$root/src/hybrid_fusion.cpp"
require "Locked low-latency pacing source for session" "$root/src/low_latency.cpp"
require "sleep_with_frame_id" "$root/src/low_latency_d3d.cpp"
require "exact_frame_pacing_" "$root/src/low_latency_tech/ll_xell.cpp"
require "non-pacing frontends are still legitimate fixed" "$root/src/low_latency_d3d.cpp"
require "frontend_requested_enabled" "$root/src/low_latency.h"
require "frontend_requested_enabled" "$root/src/low_latency_d3d.cpp"

# GCC/fmt12 regression guards from the first real r1 MinGW target build.
if grep -Eq 'spdlog::info\([[:space:]]*first_lock_application[[:space:]]*\?' "$root/src/low_latency.cpp"; then
    echo "FAIL runtime-selected fmt string reintroduced" >&2
    exit 1
fi
if grep -Eq 'frontend_index\(|kSleepModeValid|kSleepModeEnabled' "$root/src/low_latency_d3d.cpp"; then
    echo "FAIL low_latency_d3d.cpp references low_latency.cpp translation-unit internals" >&2
    exit 1
fi

if grep -Fq 'select_aspect_source(' "$root/src/hybrid_fusion.cpp"; then
    echo "FAIL runtime-adaptive aspect selector still present" >&2
    exit 1
fi

echo "PASS Startup-locked Hybrid execution architecture audit"
