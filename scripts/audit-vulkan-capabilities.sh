#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
H="$root/src/vulkan_capabilities.h"
C="$root/src/vulkan_capabilities.cpp"
HOOK="$root/src/vulkan_hooks.cpp"
REGH="$root/src/vulkan_device_registry.h"
REGC="$root/src/vulkan_device_registry.cpp"
VKD3D="$root/src/vkd3d_vulkan_capabilities.cpp"
DXVK="$root/src/dxvk_vulkan_interop.cpp"
VF="$root/src/low_latency_tech/ll_vulkanflex.cpp"
MESON="$root/src/meson.build"
TEST="$root/tests/vulkan_capabilities_test.cpp"

for f in "$H" "$C" "$HOOK" "$REGH" "$REGC" "$VKD3D" "$DXVK" "$VF" "$MESON" "$TEST"; do test -f "$f"; done

# Three-state capability contract: physical support is distinct from concrete
# device enabled-state, and unknown must remain representable.
grep -q 'std::uint64_t supported' "$H"
grep -q 'std::uint64_t enabled' "$H"
grep -q 'std::uint64_t enabled_known' "$H"
grep -q 'support_probe_complete' "$H"

# Version model and EDS granularity. EDS2 optional bits must never be collapsed
# into the Vulkan-1.3 core EDS2 bit; EDS3 is an individual-feature mask.
grep -q 'Vulkan13' "$H"
grep -q 'Vulkan14' "$H"
grep -q 'ExtendedDynamicState2LogicOp' "$H"
grep -q 'ExtendedDynamicState2PatchControlPoints' "$H"
grep -q 'enum class VulkanEds3Feature' "$H"
grep -q 'ShadingRateImageEnable' "$H"
grep -q 'eds3_supported' "$H"
grep -q 'eds3_enabled_known' "$H"
grep -q 'extendedDynamicState2LogicOp' "$C"
grep -q 'extendedDynamicState2PatchControlPoints' "$C"
grep -q 'eds3_mask_from' "$C"

# Engine is compiled and carried through the device registry.
grep -q "'vulkan_capabilities.cpp'" "$MESON"
grep -q 'VulkanCapabilitySnapshot capabilities' "$REGH"
grep -q 'capability_supported' "$REGC"
grep -q 'get_capabilities' "$REGC"

# Native Vulkan has exact VkDeviceCreateInfo knowledge.
grep -q 'probe_vulkan_capability_support' "$HOOK"
grep -q 'observe_vulkan_device_create_info' "$HOOK"
grep -q 'state.capabilities = capability_snapshot' "$HOOK"

# VKD3D public interop exposes enabled extension/features and therefore may
# merge exact enabled-state. DXVK public interop is support-only by design.
grep -q 'GetDeviceExtensions' "$VKD3D"
grep -q 'GetDeviceFeatures' "$VKD3D"
grep -q 'observe_vulkan_enabled_state' "$VKD3D"
grep -q 'probe_vulkan_capability_support' "$DXVK"
! grep -q 'observe_vulkan_enabled_state' "$DXVK"
! grep -q 'observe_vulkan_device_create_info' "$DXVK"

# Translation capability snapshots are telemetry/control-plane data only.
grep -q 'transport_capabilities_ = caps.capabilities' "$VF"
grep -q 'transport_capabilities_ = {}' "$VF"

# R3.9 must not become a renderer or queue owner. Capability discovery may
# inspect VkDeviceCreateInfo but must never mutate it or submit GPU work.
! grep -qE 'vkCreateGraphicsPipelines|vkCmd[A-Z]|vkQueueSubmit|vkQueueSubmit2|vkQueuePresentKHR|vkAcquireNextImage|vkWaitForPresent|vkGetPastPresentationTiming' "$C"
! grep -qE 'const_cast|reinterpret_cast<VkDeviceCreateInfo|ppEnabledExtensionNames\s*=|pEnabledFeatures\s*=' "$C"

# Unit test explicitly covers Vulkan 1.4, Vulkan 1.3, support-only unknown
# state and EDS3 extension-family vs individual-feature semantics.
grep -q 'VK_API_VERSION_1_4' "$TEST"
grep -q 'g_api_version = VK_API_VERSION_1_3' "$TEST"
grep -q '!support.knows_enabled(VulkanCapability::ExtendedDynamicState3)' "$TEST"
grep -q 'eds3_enabled == 0' "$TEST"

echo 'PASS: VulkanFlex R3.9 transport-neutral passive Vulkan capability engine audit'
