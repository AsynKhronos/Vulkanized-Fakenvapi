#include "runtime_policy.h"

namespace policy {

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

template <typename T, std::size_t N>
void hash_order(std::uint64_t& hash, const FixedOrder<T, N>& order) noexcept {
    hash_byte(hash, order.size);
    for (std::uint8_t i = 0; i < order.size; ++i) {
        hash_byte(hash, static_cast<std::uint8_t>(order.values[i]));
    }
}

} // namespace

std::uint64_t compute_routing_signature(const RuntimePolicySnapshot& snapshot) noexcept {
    std::uint64_t hash = kFnvOffset;

    hash_byte(hash, snapshot.general.allow_fallback);
    hash_byte(hash, snapshot.general.runtime_switching);
    hash_byte(hash, static_cast<std::uint8_t>(snapshot.input.mode));
    hash_byte(hash, snapshot.input.minimum_quality);
    hash_order(hash, snapshot.input.priority);

    hash_byte(hash, static_cast<std::uint8_t>(snapshot.output.d3d11));
    hash_byte(hash, static_cast<std::uint8_t>(snapshot.output.d3d12));
    hash_byte(hash, static_cast<std::uint8_t>(snapshot.output.vulkan));
    hash_order(hash, snapshot.backend_order.d3d11);
    hash_order(hash, snapshot.backend_order.d3d12);
    hash_order(hash, snapshot.backend_order.vulkan);

    hash_byte(hash, snapshot.hybrid.xell_fusion);
    hash_byte(hash, snapshot.hybrid.vulkan_fusion);
    hash_byte(hash, snapshot.hybrid.startup_locked);
    hash_byte(hash, static_cast<std::uint8_t>(snapshot.hybrid.startup_observations & 0xffu));
    hash_byte(hash, static_cast<std::uint8_t>((snapshot.hybrid.startup_observations >> 8) & 0xffu));
    hash_byte(hash, snapshot.hybrid.aspect_adaptive);
    hash_byte(hash, snapshot.hybrid.aspect_minimum_quality);
    hash_byte(hash, snapshot.hybrid.aspect_switch_margin);
    hash_byte(hash, snapshot.hybrid.fill_missing_signals);
    hash_byte(hash, snapshot.hybrid.secondary_minimum_quality);

    hash_byte(hash, static_cast<std::uint8_t>(snapshot.vulkan.spoofing));
    hash_byte(hash, static_cast<std::uint8_t>(snapshot.vulkan.expose_nv_low_latency2));
    hash_byte(hash, static_cast<std::uint8_t>(snapshot.vulkan.expose_amd_anti_lag));
    hash_byte(hash, snapshot.vulkan.prefer_native_extensions);

    hash_byte(hash, snapshot.vulkanflex.enabled);
    hash_byte(hash, snapshot.vulkanflex.core_pacing);
    hash_byte(hash, snapshot.vulkanflex.only_native_vulkan);
    hash_byte(hash, snapshot.vulkanflex.vkd3d_bridge);
    hash_byte(hash, snapshot.vulkanflex.dxvk_bridge);
    hash_byte(hash, static_cast<std::uint8_t>(snapshot.vulkanflex.dxvk_execution));
    hash_byte(hash, static_cast<std::uint8_t>(snapshot.vulkanflex.present_precision));
    hash_byte(hash, static_cast<std::uint8_t>(snapshot.vulkanflex.queue_pressure));
    hash_byte(hash, snapshot.vulkanflex.queue_target_presents);
    hash_byte(hash, static_cast<std::uint8_t>(snapshot.vulkanflex.structural_stall));
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash_byte(hash, static_cast<std::uint8_t>((snapshot.vulkanflex.queue_max_delay_us >> shift) & 0xffu));
        hash_byte(hash, static_cast<std::uint8_t>((snapshot.vulkanflex.pipeline_threshold_us >> shift) & 0xffu));
        hash_byte(hash, static_cast<std::uint8_t>((snapshot.vulkanflex.stall_max_compensation_us >> shift) & 0xffu));
    }
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash_byte(hash, static_cast<std::uint8_t>((snapshot.vulkanflex.minimum_interval_us >> shift) & 0xffu));
        hash_byte(hash, static_cast<std::uint8_t>((snapshot.vulkanflex.max_sleep_us >> shift) & 0xffu));
    }

    // Compatibility settings that still affect execution in 0.2.
    hash_byte(hash, snapshot.legacy.force_reflex);
    hash_byte(hash, snapshot.legacy.latencyflex_mode);

    return hash;
}

} // namespace policy
