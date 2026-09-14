#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
H="$ROOT/src/hybrid_fusion.h"
C="$ROOT/src/hybrid_fusion.cpp"
L="$ROOT/src/low_latency.cpp"
LH="$ROOT/src/low_latency.h"
D="$ROOT/src/low_latency_d3d.cpp"
VF="$ROOT/src/low_latency_tech/ll_vulkanflex.cpp"
VFH="$ROOT/src/low_latency_tech/ll_vulkanflex.h"
POL="$ROOT/src/policy_engine.cpp"
X="$ROOT/src/low_latency_tech/ll_xell.cpp"
XH="$ROOT/src/low_latency_tech/ll_xell.h"

require() {
    local needle="$1" file="$2"
    grep -Fq "$needle" "$file" || { echo "FAIL missing '$needle' in $file" >&2; exit 1; }
}
reject() {
    local needle="$1" file="$2"
    if grep -Fq "$needle" "$file"; then
        echo "FAIL forbidden '$needle' in $file" >&2
        exit 1
    fi
}

# Canonical FrameToken is independent of native frontend frame-ID domains and
# uses fixed O(1) source bindings. No map/vector/heap lookup belongs here.
require "struct CanonicalFrameToken" "$H"
require "source_frame_ids" "$H"
require "source_bindings_" "$H"
require "next_token_sequence_" "$H"
require "canonicalize_pacing_observation" "$C"
require "canonicalize_marker_locked" "$C"
require "bind_source_to_latest" "$C"
require "Native frontend IDs are deliberately not" "$H"
reject "std::unordered_map" "$H"
reject "std::map" "$H"
reject "std::vector" "$H"

# The canonical sequence, not a foreign Reflex/AL2/XeLL ID, is sent to the
# execution backend.
require "*effective_frame_id = token->sequence" "$L"
require "marker_params.frame_id = canonical_frame_id" "$D"

# D3D12 arbitration prefers mature D3D12 execution. VulkanFlex remains a
# fallback executor and attaches independently as a VKD3D observer.
require "XeLL remains the preferred software executor" "$POL"
require "recognized_usable(Backend::AntiLag2)" "$POL"
require "recognized_available(Backend::VulkanFlex)" "$POL"
require "Role : std::uint8_t { Executor, Observer, Delegate }" "$VFH"
require "init_observer" "$VFH"
require "if (role_ == Role::Observer) return;" "$VF"
require "if (role_ == Role::Delegate)" "$VF"
require "dxvk_low_latency_->LatencySleep()" "$VF"
require "VulkanFlex translation observer attached behind executor" "$D"
require "transport_observer_storage_" "$LH"
require "transport_observer_published_" "$D"
reject "auto* observer = new VulkanFlex" "$D"

# XeLL lifecycle is context-local and exact-frame correlated. A modulo slot may
# not remain a sticky bool across unrelated FrameTokens, and unloading the DLL
# must invalidate its handle before a later backend reinitialization.
require "sent_sleep_frame_ids_" "$XH"
require "!= frame_id" "$X"
require "xell_dll = nullptr" "$X"
require "dx12_pDevice->Release()" "$X"
require "last_sleep_mode_valid_" "$XH"
reject "static uint32_t last_bLowLatencyMode" "$X"

# Observer mode is transport telemetry only: never inject Vulkan submissions or
# take VKD3D queue locks in the pacing hot path.
! grep -qE 'vkQueueSubmit|BeginVkCommandBufferInterop|LockVulkanQueue|LockCommandQueue' "$VF"

echo "PASS: VulkanFlex D3D12 executor arbitration + canonical FrameToken observer architecture"
