#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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

B="$ROOT/src/low_latency_tech/ll_vkd3d_antilag_vk.cpp"
V="$ROOT/src/vkd3d_vulkan_capabilities.cpp"
VH="$ROOT/src/vkd3d_vulkan_capabilities.h"
H="$ROOT/src/low_latency_tech/ll_vkd3d_antilag_vk.h"
I="$ROOT/src/vkd3d_vulkan_interop.h"
D="$ROOT/src/low_latency_d3d.cpp"
L="$ROOT/src/low_latency.cpp"
P="$ROOT/src/policy_engine.cpp"
R="$ROOT/src/runtime_policy.h"
M="$ROOT/src/meson.build"
C="$ROOT/src/config.cpp"

# Exact vkd3d-proton public interop bridge, no new link-time dependency.
require "39da4e09" "$I"
require "GetDeviceExtensions" "$I"
require "GetDeviceFeatures" "$I"
require "GetVulkanHandles" "$I"
require "GetVulkanQueueInfo" "$I"
require "90ecf26e" "$I"
require "ll_vkd3d_antilag_vk.cpp" "$M"
require "vkd3d_vulkan_capabilities.cpp" "$M"

# Direct AMD Vulkan capability gate: centralized vkd3d/Vulkan probe + device proc.
require "probe_vkd3d_vulkan_capabilities" "$B"
require "VK_AMD_ANTI_LAG_EXTENSION_NAME" "$V"
require "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD" "$V"
require "antiLag == VK_TRUE" "$V"
require "vkGetPhysicalDeviceQueueFamilyProperties" "$V"
require "VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME" "$V"
require "VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME" "$V"
require "vkGetDeviceProcAddr" "$B"
require "vkAntiLagUpdateAMD" "$B"
require "pPresentationInfo = nullptr" "$B"
require "VK_ANTI_LAG_STAGE_INPUT_AMD" "$B"
require "VK_ANTI_LAG_STAGE_PRESENT_AMD" "$B"

# Keep this backend below D3D and vkd3d interop: it must not create/hook Vulkan.
reject "vkCreateDevice(" "$B"
reject "DetourAttach" "$B"
reject "hook_vulkan" "$B"
reject "std::vector" "$B"
reject "std::vector" "$V"

# No speculative direct NV LL2 from D3D12: those commands need a real VkSwapchainKHR.
reject "vkLatencySleepNV" "$B"
reject "vkSetLatencySleepModeNV" "$B"

# Policy only injects direct D3D12 Vulkan execution when explicitly preferred.
require "snapshot.vulkan.prefer_native_extensions" "$P"
require "result.order.push_back(Backend::AmdAntiLagVk)" "$P"
if awk '/BackendOrder d3d12 =/{f=1} f&&/AmdAntiLagVk/{bad=1} f&&/return value;/{exit} END{exit bad?0:1}' "$R"; then
    echo "FAIL AmdAntiLagVk leaked into unconditional D3D12 fallback order" >&2
    exit 1
fi

# Stable execution path: route lookup and unchanged controls are atomic fast paths.
require "active_backend_published_.load" "$D"
require "applied_routing_signature_published_.load" "$D"
require "Steady-state D3D hot path" "$D"
require "applied_fg_state_.load" "$L"
require "applied_override_.load" "$L"
require "hybrid_fusion_.locked()" "$L"
require "sleep_with_frame_id(effective_frame_id)" "$D"
require "latency_not_ready_logged_.exchange" "$D"
require "suppressing repeated empty-report messages" "$D"
require "Hybrid startup fusion" "$C"
require "#define VFN_HOT_TRACE(...) do { } while (0)" "$ROOT/src/log.h"
require "VFN_HOT_TRACE" "$ROOT/src/log.cpp"
require "VFN_HOT_TRACE" "$ROOT/src/low_latency_d3d.cpp"
require "VFN_HOT_TRACE" "$ROOT/src/low_latency_vk.cpp"

# Direct backend's mutable control state is lock-free and integer-only.
require "std::atomic<bool> requested_enabled_" "$H"
require "std::atomic<std::uint32_t> max_fps_" "$H"
reject "std::round" "$B"
reject "<cmath>" "$B"

# Startup fusion must establish its execution backend before evidence selection,
# otherwise the first marker could accidentally take the legacy selector path.
require "startup_hybrid_configured" "$D"
require "Establish the single execution backend before startup evidence arrives" "$D"

require "vkd3d-proton Vulkan interop detected" "$B"
require "VK_AMD_anti_lag unavailable (extension={}, feature={}); falling back" "$B"

echo "PASS low-level execution and hot-path audit"
