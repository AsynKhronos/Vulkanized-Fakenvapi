#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VF="$root/src/low_latency_tech/ll_vulkanflex.cpp"
VFH="$root/src/low_latency_tech/ll_vulkanflex.h"
HOOK="$root/src/vulkan_hooks.cpp"
REG="$root/src/vulkan_device_registry.cpp"
PREC="$root/src/vulkan_present_precision.cpp"
POL="$root/src/runtime_policy.h"
CFG="$root/fakenvapi.ini"
MESON="$root/src/meson.build"

for f in "$VF" "$VFH" "$HOOK" "$REG" "$PREC" "$POL" "$CFG" "$MESON"; do test -f "$f"; done

grep -q 'PresentPrecisionTier::PresentId' "$PREC"
grep -q 'PresentPrecisionTier::PresentWait' "$PREC"
grep -q 'PresentPrecisionTier::PresentWait2' "$PREC"
grep -q 'PresentPrecisionTier::PresentTiming' "$PREC"
grep -q 'vkWaitForPresentKHR' "$HOOK"
grep -q 'vkWaitForPresent2KHR' "$HOOK"
grep -q 'vkGetPastPresentationTimingEXT' "$HOOK"
grep -q 'hkvkCreateSwapchainKHR' "$HOOK"
grep -q 'register_swapchain' "$HOOK"
# Present metadata pNext traversal is once per vkQueuePresentKHR, not once per swapchain.
grep -q 'const auto metadata_view = parse_present_metadata(\*present_info);' "$HOOK"
grep -q 'present_metadata_at(\*present_info, metadata_view, index)' "$HOOK"
grep -q 'present_precision=auto' "$CFG"
grep -q "'vulkan_present_precision.cpp'" "$MESON"
# R3.6 is sensor-only: completion probes must remain non-blocking.
grep -q 'wait_info.timeout = 0' "$VF"
grep -q 'wait_for_present(device, swapchain, present_id, 0)' "$VF"
# Empty precision queues must bypass registry/slot polling on the acquire hot path.
grep -q 'precision_pending_count.load(std::memory_order_relaxed) == 0' "$VF"
grep -q 'precision_pending_count.fetch_add(1, std::memory_order_relaxed)' "$VF"
grep -q 'kPrecisionSlotPublishing' "$VF"
grep -q 'kPrecisionWaitProbeBudget' "$VFH"
grep -q 'timing_probe_needed' "$VF"
grep -q 'precision_profile_state' "$VFH"
# It may observe application metadata but must not add present precision extensions.
! grep -qE 'extensions\.push_back\((VK_KHR_PRESENT_ID|VK_KHR_PRESENT_WAIT|VK_KHR_PRESENT_WAIT_2|VK_EXT_PRESENT_TIMING)' "$HOOK"
# Queue-depth blocking and injected GPU work belong to R3.7+, not this layer.
! grep -qE 'vkQueueSubmit|vkQueueSubmit2' "$VF"

echo 'PASS: VulkanFlex R3.6 passive present-precision ladder audit'
