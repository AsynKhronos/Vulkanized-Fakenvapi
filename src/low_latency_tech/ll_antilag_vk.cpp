#include "ll_antilag_vk.h"

#include "vulkan_device_registry.h"

#include <cstddef>

bool AntiLagVk::init(IUnknown* pDevice) {
    const auto device = reinterpret_cast<VkDevice>(pDevice);
    return VulkanDeviceRegistry::has_amd_anti_lag(device);
}

// Unsupported
bool AntiLagVk::init_using_ctx(void* context) {
    spdlog::error("AntiLagVk init_using_ctx is not supported");
    inited_using_context = false;
    return false;
}

void* AntiLagVk::get_tech_context() {
    return nullptr;
}


void AntiLagVk::rebuild_hot_control() noexcept {
    const auto override_mode = override_mode_.load(std::memory_order_acquire);
    const bool enabled = override_mode != ForceReflex::InGame
        ? override_mode == ForceReflex::ForceEnable
        : requested_enabled_.load(std::memory_order_acquire);
    const auto max_fps = max_fps_.load(std::memory_order_relaxed);
    hot_control_.store(
        static_cast<std::uint64_t>(max_fps) | (enabled ? kHotEnabled : 0),
        std::memory_order_release);
}

void AntiLagVk::set_low_latency_override(ForceReflex value) {
    override_mode_.store(value, std::memory_order_release);
    rebuild_hot_control();
}

void AntiLagVk::get_sleep_status(SleepParams* sleep_params) {
    sleep_params->low_latency_enabled = is_enabled();
    sleep_params->fullscreen_vrr = true;
    sleep_params->control_panel_vsync_override = false;
}

void AntiLagVk::set_sleep_mode(SleepMode* sleep_mode) {
    // UNUSED:
    // low_latency_boost
    // use_markers_to_optimize

    requested_enabled_.store(sleep_mode->low_latency_enabled, std::memory_order_release);
    const auto interval = sleep_mode->minimum_interval_us;
    const std::uint32_t max_fps = interval > 0
        ? static_cast<std::uint32_t>((1'000'000ull + interval / 2u) / interval)
        : 0u;
    max_fps_.store(max_fps, std::memory_order_release);
    rebuild_hot_control();
}

void AntiLagVk::sleep() {
    // Vulkan Anti-Lag is driven by input/present markers rather than a separate
    // sleep call. The NVAPI timeline semaphore is handled by the Vulkan device
    // registry independently.
}

void AntiLagVk::set_marker(IUnknown* pDevice, MarkerParams* marker_params) {
    const auto device = reinterpret_cast<VkDevice>(pDevice);
    const auto anti_lag_update = VulkanDeviceRegistry::get_anti_lag_update(device);
    if (!anti_lag_update)
        return;

    const auto hot_control = hot_control_.load(std::memory_order_acquire);
    const bool enabled = (hot_control & kHotEnabled) != 0;
    const auto mode = enabled ? VK_ANTI_LAG_MODE_ON_AMD : VK_ANTI_LAG_MODE_OFF_AMD;
    const auto max_fps = static_cast<std::uint32_t>(hot_control);

    static thread_local size_t call_count = 0;
    static thread_local size_t last_oob_present = 0;
    static thread_local bool using_oob_present = false;
    constexpr size_t allowed_gap = 10;

    ++call_count;

    if (marker_params->marker_type == MarkerType::OUT_OF_BAND_PRESENT_START) {
        last_oob_present = call_count;
        using_oob_present = true;
    }

    if (using_oob_present && call_count - last_oob_present > allowed_gap)
        using_oob_present = false;

    if (marker_params->marker_type == MarkerType::SIMULATION_START) {
        VkAntiLagPresentationInfoAMD input_info{};
        input_info.sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD;
        input_info.stage = VK_ANTI_LAG_STAGE_INPUT_AMD;
        input_info.frameIndex = marker_params->frame_id;

        VkAntiLagDataAMD anti_lag_data{};
        anti_lag_data.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
        anti_lag_data.mode = mode;
        anti_lag_data.pPresentationInfo = &input_info;
        anti_lag_data.maxFPS = max_fps;

        VFN_HOT_TRACE("AntiLag Input: {}, status: {}", marker_params->frame_id, enabled);
        anti_lag_update(device, &anti_lag_data);
    }

    if ((marker_params->marker_type == MarkerType::PRESENT_START && !using_oob_present) ||
        marker_params->marker_type == MarkerType::OUT_OF_BAND_PRESENT_START) {
        VkAntiLagPresentationInfoAMD present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD;
        present_info.stage = VK_ANTI_LAG_STAGE_PRESENT_AMD;
        present_info.frameIndex = marker_params->frame_id;

        VkAntiLagDataAMD anti_lag_data{};
        anti_lag_data.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
        anti_lag_data.mode = mode;
        anti_lag_data.pPresentationInfo = &present_info;
        anti_lag_data.maxFPS = max_fps;

        VFN_HOT_TRACE("AntiLag Present: {}, status: {}", marker_params->frame_id, enabled);
        anti_lag_update(device, &anti_lag_data);
    }
}
