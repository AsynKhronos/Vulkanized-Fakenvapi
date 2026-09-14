#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ABI="$ROOT/src/dxvk_vulkan_interop.h"
PROBE="$ROOT/src/dxvk_vulkan_interop.cpp"
VF="$ROOT/src/low_latency_tech/ll_vulkanflex.cpp"
VFH="$ROOT/src/low_latency_tech/ll_vulkanflex.h"
D3D="$ROOT/src/low_latency_d3d.cpp"
POL="$ROOT/src/policy_engine.cpp"
CFG="$ROOT/src/runtime_policy.h"
MESON="$ROOT/src/meson.build"

for f in "$ABI" "$PROBE" "$VF" "$VFH" "$D3D" "$POL" "$CFG" "$MESON"; do test -f "$f"; done

grep -q '0xe2ef5fa5, 0xdc21, 0x4af7' "$ABI"
grep -q '0xf3112584, 0x41f9, 0x348d' "$ABI"
grep -q 'GetVulkanHandles' "$ABI"
grep -q 'GetSubmissionQueue' "$ABI"
grep -q 'SupportsLowLatency' "$ABI"
grep -q 'LatencySleep' "$ABI"
grep -q 'SetLatencySleepMode' "$ABI"
grep -q 'SetLatencyMarker' "$ABI"
grep -q "'dxvk_vulkan_interop.cpp'" "$MESON"
grep -q 'Transport::DxvkD3D11' "$VF"
grep -q 'Role::Delegate' "$VF"
grep -q 'dxvk_execution = AutoBool::Auto' "$CFG"
grep -q 'probe_dxvk_vulkan_capabilities' "$D3D"
grep -q 'FrameTransport::DxvkD3D11' "$D3D"
grep -q 'GraphicsApi::D3D11' "$POL"
# Cooperative means no injected Vulkan work and no DXVK submission-queue lock.
! grep -qE 'LockSubmissionQueue|ReleaseSubmissionQueue|vkQueueSubmit|vkQueueSubmit2' "$VF"

echo 'PASS: VulkanFlex DXVK cooperative bridge and single-pacing-owner audit'
