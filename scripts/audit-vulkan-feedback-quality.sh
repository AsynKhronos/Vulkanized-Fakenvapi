#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cpp="$root/src/low_latency_tech/ll_vulkanflex.cpp"
hdr="$root/src/low_latency_tech/ll_vulkanflex.h"
policy="$root/src/completion_feedback_policy.h"

for f in "$cpp" "$hdr" "$policy"; do test -f "$f"; done

grep -q 'kPrecisionMaxPolls = 6' "$hdr"
grep -q 'completionfeedback::Candidate feedback_batch' "$cpp"
grep -q 'queue_completed_frames_\.fetch_add(completion_count' "$cpp"
grep -q 'rejected_feedback_\.fetch_add(1' "$cpp"
grep -q 'feedback.timestamp_ns > previous_ns && feedback.frame_id > previous_frame' "$cpp"
grep -q 'precision_pending_count.compare_exchange_weak' "$cpp"
grep -q 'precise && !candidate.precise' "$policy"

echo 'PASS: VulkanFlex completion-feedback quality / ordering audit'
