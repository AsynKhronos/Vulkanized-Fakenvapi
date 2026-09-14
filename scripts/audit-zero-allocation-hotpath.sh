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

R="$ROOT/src/vulkan_device_registry.cpp"
RH="$ROOT/src/vulkan_device_registry.h"
L="$ROOT/src/low_latency.cpp"
D="$ROOT/src/low_latency_d3d.cpp"
V="$ROOT/src/low_latency_vk.cpp"
VH="$ROOT/src/vulkan_hooks.cpp"
H="$ROOT/src/hybrid_fusion.cpp"
A="$ROOT/src/low_latency_tech/ll_antilag_vk.cpp"
AH="$ROOT/src/low_latency_tech/ll_antilag_vk.h"
VA="$ROOT/src/low_latency_tech/ll_vkd3d_antilag_vk.cpp"
VAH="$ROOT/src/low_latency_tech/ll_vkd3d_antilag_vk.h"

# Fixed-capacity read-mostly registry. No heap containers or reader mutex.
require "struct AtomicDeviceSlot" "$R"
require "struct AtomicQueueSlot" "$R"
require "thread_local ThreadLocalDeviceCache" "$R"
require "thread_local ThreadLocalQueueCache" "$R"
require "Even = stable snapshot, odd = a writer" "$R"
require "cached_device_state" "$R"
require "native_notify" "$R"
require "queue_present" "$R"
require "get_queue_present_dispatch" "$R"
require "get_acquire_next_image" "$R"
reject "std::vector" "$R"
reject "std::unordered_map" "$R"
reject "std::map" "$R"
reject "g_devices_mutex" "$R"

# Compact native dispatch instead of full device-state copies in VK LL2 calls.
require "VulkanNvLowLatency2Dispatch" "$RH"
require "get_nv_low_latency2_dispatch" "$VH"

# Emulated latency sleep uses a bounded allocation-free MPMC queue. No producer
# spinlock and no unconditional per-frame kernel event transition.
require "FixedMpmcRing<EmulatedSleepJob" "$VH"
require "g_sleep_worker_waiting.load" "$VH"
require "g_sleep_worker_waiting.compare_exchange_strong" "$VH"
require "Avoid a locked RMW on the steady producer path" "$VH"
reject "g_sleep_worker_waiting.exchange(false" "$VH"
reject "g_sleep_producer_lock" "$VH"
reject "g_sleep_write" "$VH"
reject "g_sleep_read" "$VH"

# Backend lookup must have lock-free published steady-state routes.
require "Steady-state D3D hot path" "$D"
require "Vulkan steady-state fast path" "$V"
require "cached_d3d_api" "$D"
require "active_vk_device_published_" "$V"
require "active_tech_published_" "$D"
require "active_tech_published_" "$V"

# Startup arbitration disappears after the frozen Hybrid ownership decision.
require "hybrid_hotpath_locked_" "$L"
require "canonicalize_marker_locked" "$L"
require "canonicalize_marker_locked" "$H"
require "source_bindings_" "$H"
require "Do not keep paying quality/freshness telemetry" "$L"

# Control-plane state is packed to one marker-hot 64-bit load.
require "hot_control_.load" "$A"
require "hot_control_.load" "$VA"
require "kHotEnabled" "$AH"
require "kHotEnabled" "$VAH"

# Marker timestamp accounting is one clock read per callback, not one per field.
require "Exactly one clock read per marker" "$V"
timestamp_count="$(awk '/void LowLatency::add_vulkan_marker_to_report/{f=1} f{print} /void LowLatency::get_vulkan_latency_timings/{exit}' "$V" \
    | grep -c 'get_timestamp()' || true)"
if [[ "$timestamp_count" -ne 1 ]]; then
    echo "FAIL expected exactly one Vulkan timestamp read per marker callback, found $timestamp_count" >&2
    exit 1
fi


# VulkanFlex steady-state flags/counters must not perform unconditional locked
# RMW operations when the overwhelmingly common state is false/zero/unchanged.
VF="$ROOT/src/low_latency_tech/ll_vulkanflex.cpp"
require "reset_requested_.load" "$VF"
require "queue_pressure_delay_ns_.load" "$VF"
require "pressure_epoch.load" "$VF"
require "pressure_epoch.compare_exchange_weak" "$VF"
require "core_paced_pending_.load" "$VF"
require "frame_sequence_.load(std::memory_order_relaxed) + 1" "$VF"
reject "frame_sequence_.fetch_add" "$VF"

# Release hot logging must compile away completely.
require "#define VFN_HOT_TRACE(...) do { } while (0)" "$ROOT/src/log.h"

echo "PASS: zero-allocation / lock-free steady-state hot-path architecture audit"
