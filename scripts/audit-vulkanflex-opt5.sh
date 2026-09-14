#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VF="$root/src/low_latency_tech/ll_vulkanflex.cpp"
VFH="$root/src/low_latency_tech/ll_vulkanflex.h"
HOOK="$root/src/vulkan_hooks.cpp"
LLVK="$root/src/low_latency_vk.cpp"

for f in "$VF" "$VFH" "$HOOK" "$LLVK"; do test -f "$f"; done

# Two-phase present correlation must claim before the real driver call and only
# publish completion after that call returns successfully.
claim_line=$(grep -n 'VulkanFlexBeginPresent(' "$HOOK" | head -1 | cut -d: -f1)
driver_line=$(grep -n 'const VkResult result = present(queue, present_info);' "$HOOK" | head -1 | cut -d: -f1)
complete_line=$(grep -n 'VulkanFlexCompletePresent(' "$HOOK" | head -1 | cut -d: -f1)
[[ -n "$claim_line" && -n "$driver_line" && -n "$complete_line" ]]
(( claim_line < driver_line && driver_line < complete_line ))

grep -q 'std::array<ClaimedPresent, VulkanFlex::kTrackedSwapchainLanes>' "$HOOK"
grep -q 'frame_ids\[image_index\]\.exchange' "$VF"
grep -q 'frame_epochs\[image_index\]\.load' "$VF"
! grep -q 'frame_epochs\[image_index\]\.exchange' "$VF"

# A lane identity is release-published only after payload state is reset.
init_line=$(grep -n 'selected->precision_pending_count.store' "$VF" | head -1 | cut -d: -f1)
publish_line=$(grep -n 'selected->swapchain.store(swapchain, std::memory_order_release)' "$VF" | head -1 | cut -d: -f1)
(( init_line < publish_line ))
grep -q 'lane_create_gate_' "$VFH"

# Precision profile is immutable/cached per lane; steady probes do not need a
# fresh swapchain-registry walk on every acquire.
grep -q 'precision_profile_state' "$VFH"
grep -q 'ensure_precision_profile' "$VF"
grep -q 'const auto& dispatch = lane->precision_dispatch' "$VF"

# Free-slot-first precision ring replaces the unconditional round-robin RMW.
grep -q 'kPrecisionSlotPublishing' "$VF"
grep -q 'candidate.present_id.load(std::memory_order_acquire) != 0' "$VF"
grep -q 'Only a genuinely full ring may evict' "$VF"
! grep -q 'precision_sequence' "$VF"

# Driver re-entry is bounded and present-timing is queried only when requested.
grep -q 'kPrecisionWaitProbeBudget = 4' "$VFH"
grep -q 'driver_probes < kPrecisionWaitProbeBudget' "$VF"
grep -q 'timing_probe_needed' "$VF"

# Preserve the no-injected-work ownership contract.
! grep -qE 'vkQueueSubmit|vkQueueSubmit2|vkQueueWaitIdle|vkDeviceWaitIdle' "$VF"

echo 'PASS: VulkanFlex Opt5 two-phase WSI / cached precision / bounded probe audit'
