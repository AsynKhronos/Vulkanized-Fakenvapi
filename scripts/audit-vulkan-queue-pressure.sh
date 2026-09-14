#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VF="$root/src/low_latency_tech/ll_vulkanflex.cpp"
VFH="$root/src/low_latency_tech/ll_vulkanflex.h"
GOV="$root/src/vulkan_queue_pressure.cpp"
POL="$root/src/runtime_policy.h"
CFG="$root/fakenvapi.ini"
MESON="$root/src/meson.build"

for f in "$VF" "$VFH" "$GOV" "$POL" "$CFG" "$MESON"; do test -f "$f"; done

grep -q 'queue_pressure=auto' "$CFG"
grep -q 'queue_target_presents=1' "$CFG"
grep -q 'queue_max_delay_us=2000' "$CFG"
grep -q 'select_queue_pressure_delay' "$VF"
grep -q 'queue_pressure_delay_ns_' "$VFH"
grep -q 'transport_ == Transport::NativeVulkan' "$VF"
grep -q 'role_ == Role::Executor' "$VF"
grep -q 'std::max(latencyflex_target, governor_target)' "$VF"
grep -q 'if (pending_hint <= target_hint)' "$VF"
grep -q "'vulkan_queue_pressure.cpp'" "$MESON"
# R3.7 soft governor must not add another Vulkan submission or a blocking
# present-wait path. Blocking policy can only be evaluated after wider runtime sign-off.
! grep -qE 'vkQueueSubmit|vkQueueSubmit2' "$GOV" "$VF"
! grep -qE 'wait_info\.timeout = [1-9]|wait_for_present\([^,]+,[^,]+,[^,]+,[[:space:]]*[1-9]' "$VF"
# Translation stacks remain observer/delegate and never use the native governor.
grep -q 'queue_pressure_enabled_ = role_ == Role::Executor &&' "$VF"

echo 'PASS: VulkanFlex R3.7 soft queue-pressure/single-owner audit'
