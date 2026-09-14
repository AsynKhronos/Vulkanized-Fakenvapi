#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

require() { grep -Fq "$1" "$2" || { echo "FAIL missing '$1' in $2" >&2; exit 1; }; }
reject() { if grep -Fq "$1" "$2"; then echo "FAIL forbidden '$1' in $2" >&2; exit 1; fi; }

P="$ROOT/src/runtime_policy.h"
PE="$ROOT/src/policy_engine.cpp"
V="$ROOT/src/low_latency_vk.cpp"
L="$ROOT/src/low_latency.cpp"
VH="$ROOT/src/vulkan_hooks.cpp"
VR="$ROOT/src/vulkan_device_registry.h"
VF="$ROOT/src/low_latency_tech/ll_vulkanflex.cpp"
VFH="$ROOT/src/low_latency_tech/ll_vulkanflex.h"

# Native-Vulkan execution contract plus translation-aware bridge routing.
require "VulkanFlex" "$P"
require "backend == Backend::VulkanFlex" "$PE"
require "case GraphicsApi::D3D11:" "$PE"
require "snapshot.vulkanflex.dxvk_bridge" "$PE"
require "snapshot.hybrid.vulkan_fusion && backend == policy::Backend::VulkanFlex" "$L"

# Core WSI interception must cover both acquire APIs, present and teardown.
require "hkvkAcquireNextImageKHR" "$VH"
require "hkvkAcquireNextImage2KHR" "$VH"
require "hkvkQueuePresentKHR" "$VH"
require "hkvkDestroySwapchainKHR" "$VH"
require "acquire_next_image" "$VR"
require "queue_present" "$VR"

# Native VK_NV_low_latency2 owns pacing in auto mode; forced VulkanFlex may override.
require "VulkanDeviceRegistry::has_native_nv_low_latency2(device)" "$V"
require "has_native_nv_low_latency2" "$VR"
require "snapshot.output.vulkan == policy::Backend::VulkanFlex" "$V"

# Embedded LatencyFleX + fixed-capacity, allocation-free hot path.
require "lfx::LatencyFleX ctx_" "$VFH"
require "FixedMpmcRing<Feedback" "$VFH"
require "std::array<SwapchainLane" "$VFH"
require "scheduler_gate_.test_and_set" "$VF"
require "fail open" "$VF"
require "frame_ids[image_index]" "$VF"
require "frame_epochs[image_index]" "$VF"
require "begin_present(" "$VF"
require "complete_present(" "$VF"
require "frame_ids[image_index].exchange" "$VF"
require "VulkanFlexBeginPresent" "$VH"
require "VulkanFlexCompletePresent" "$VH"
require "lane_create_gate_" "$VFH"
require "ensure_precision_profile" "$VF"
require "primary_swapchain_" "$VFH"
require "primary == swapchain" "$VF"
require "feedback.epoch != active_epoch" "$VF"
require "request_new_epoch()" "$VF"
require "reset_requested_.exchange" "$VF"
require "explicit_pacing_" "$VF"
require "core_paced_pending_" "$VF"
reject "new lfx::LatencyFleX" "$VF"
reject "std::vector" "$VF"
reject "std::map" "$VF"
reject "std::unordered_map" "$VF"
reject "std::mutex" "$VFH"

# Present return remains the portable fallback; R3.6 may defer it while passive
# present_wait/present_timing sensors look for stronger completion evidence.
require "const auto fallback_timestamp = get_timestamp();" "$VF"
require "publish_feedback(ticket.frame_id, fallback_timestamp, ticket.epoch)" "$VF"
require "queue_precision_feedback(" "$VF"

# Build manifest must include the implementation.
require "low_latency_tech/ll_vulkanflex.cpp" "$ROOT/src/meson.build"

echo "PASS: VulkanFlex native-WSI/translation-bridge/hybrid/zero-allocation architecture audit"
