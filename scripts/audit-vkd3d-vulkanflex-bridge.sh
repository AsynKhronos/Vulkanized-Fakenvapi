#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VF="$ROOT/src/low_latency_tech/ll_vulkanflex.cpp"
VFH="$ROOT/src/low_latency_tech/ll_vulkanflex.h"
D3D="$ROOT/src/low_latency_d3d.cpp"
POL="$ROOT/src/policy_engine.cpp"
HOOK="$ROOT/src/vulkan_hooks.cpp"
CFG="$ROOT/src/runtime_policy.h"

for f in "$VF" "$VFH" "$D3D" "$POL" "$HOOK" "$CFG"; do test -f "$f"; done

grep -q 'GetVulkanQueueInfo' "$VF"
grep -q 'IID_ID3D12DXVKInteropDevice_Value' "$VF"
grep -q 'transport_ = Transport::Vkd3dD3D12' "$VF"
grep -q 'VulkanFlex VKD3D queue bound' "$VF"
grep -q 'publish_bridge_present' "$VF"
grep -q 'set_async_marker_on_queue' "$VF"
grep -q 'observe_d3d12_queue' "$D3D"
grep -q 'Backend::VulkanFlex' "$POL"
grep -q 'vkd3d_bridge = true' "$CFG"
# R3.2 ownership protection must remain intact: bridge mode must not restore
# Vulkan detours for translation stacks.
grep -q 'Vulkan transport bypass' "$HOOK"
grep -q 'g_translation_bypass' "$HOOK"
# The cooperative bridge must not inject Vulkan work or take VKD3D queue locks.
! grep -qE 'vkQueueSubmit|BeginVkCommandBufferInterop|LockVulkanQueue|LockCommandQueue' "$VF"

echo 'PASS: VulkanFlex VKD3D cooperative bridge audit'
