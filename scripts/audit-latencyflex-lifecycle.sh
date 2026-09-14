#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
file="$ROOT/src/low_latency_tech/ll_latencyflex.cpp"
header="$ROOT/src/low_latency_tech/ll_latencyflex.h"

grep -Fq 'if (!is_enabled())' "$file" || { echo 'FAIL: LatencyFlex EndFrame disable guard missing'; exit 1; }
grep -Fq 'const bool was_enabled = is_enabled();' "$file" || { echo 'FAIL: LatencyFlex transition tracking missing'; exit 1; }
grep -Fq 'if (!was_enabled && now_enabled)' "$file" || { echo 'FAIL: LatencyFlex Off->On reset guard missing'; exit 1; }
grep -Fq 'needs_reset_.store(true' "$file" || { echo 'FAIL: LatencyFlex Off->On reset publication missing'; exit 1; }
grep -Fq 'ctx->Reset();' "$file" || { echo 'FAIL: LatencyFlex reset call missing'; exit 1; }
grep -Fq 'std::scoped_lock lock(operation_mutex_);' "$file" || { echo 'FAIL: LatencyFlex context operations are not externally synchronized'; exit 1; }
grep -Fq 'std::atomic<bool> enabled_' "$header" || { echo 'FAIL: LatencyFlex control state is not atomically published'; exit 1; }
grep -Fq 'std::uint32_t timeout_events_' "$header" || { echo 'FAIL: LatencyFlex timeout state is not per-instance'; exit 1; }
! grep -Fq 'static uint64_t timeout_events' "$file" || { echo 'FAIL: LatencyFlex timeout state still shared across instances'; exit 1; }

echo 'PASS: LatencyFlex lifecycle, per-instance state and context synchronization audit'
