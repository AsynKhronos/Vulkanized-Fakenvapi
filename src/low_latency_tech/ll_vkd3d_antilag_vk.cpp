#include "ll_vkd3d_antilag_vk.h"

#include "../vkd3d_vulkan_capabilities.h"

bool Vkd3dAntiLagVk::init(IUnknown* pDevice) {
    if (!pDevice || vk_device_ != VK_NULL_HANDLE || anti_lag_update_) return false;

    Vkd3dVulkanCapabilities caps{};
    if (!probe_vkd3d_vulkan_capabilities(pDevice, &caps) || !caps.interop) {
        spdlog::info(
            "Low-level D3D12 probe: vkd3d-proton Vulkan interop interface not exposed");
        return false;
    }

    spdlog::info(
        "Low-level D3D12 probe: vkd3d-proton Vulkan interop detected (base={}, device2={})",
        caps.interop,
        caps.interop_v2);

    if (caps.api_version != 0) {
        spdlog::info(
            "Low-level Vulkan device: api={}.{}.{}, vendor=0x{:04x}, device=0x{:04x}, driver=0x{:08x}, queues={} (gfx={}, compute={}, transfer={})",
            VK_API_VERSION_MAJOR(caps.api_version),
            VK_API_VERSION_MINOR(caps.api_version),
            VK_API_VERSION_PATCH(caps.api_version),
            caps.vendor_id,
            caps.device_id,
            caps.driver_version,
            caps.queue_family_count,
            caps.graphics_queue_families,
            caps.compute_queue_families,
            caps.transfer_queue_families);
    }

    spdlog::info(
        "Low-level Vulkan capabilities: AMD_anti_lag={}/{}, NV_low_latency2={}, timeline={}, sync2={}, present_id={}/{}, descriptor_buffer={}, device_fault={}, calibrated_timestamps={}",
        caps.amd_anti_lag_extension,
        caps.amd_anti_lag_feature,
        caps.nv_low_latency2_extension,
        caps.timeline_semaphore,
        caps.synchronization2,
        caps.present_id,
        caps.present_id2,
        caps.descriptor_buffer,
        caps.device_fault,
        caps.calibrated_timestamps);

    if (!caps.amd_anti_lag_extension || !caps.amd_anti_lag_feature) {
        spdlog::info(
            "Low-level D3D12 probe: VK_AMD_anti_lag unavailable (extension={}, feature={}); falling back",
            caps.amd_anti_lag_extension,
            caps.amd_anti_lag_feature);
        return false;
    }

    if (caps.device == VK_NULL_HANDLE) {
        spdlog::info(
            "Low-level D3D12 probe: vkd3d-proton interop did not provide a usable VkDevice; falling back");
        return false;
    }

    // Resolve through Wine's Vulkan loader only once. The concrete VkDevice is
    // borrowed from vkd3d-proton; no second VkDevice, queue or command path is
    // created by Vulkanized-Fakenvapi.
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    bool loaded_here = false;
    if (!vulkan) {
        vulkan = LoadLibraryA("vulkan-1.dll");
        loaded_here = vulkan != nullptr;
    }
    if (!vulkan) {
        spdlog::info(
            "Low-level D3D12 probe: Vulkan loader unavailable after vkd3d-proton interop detection; falling back");
        return false;
    }

    const auto get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
    if (!get_device_proc_addr) {
        spdlog::info(
            "Low-level D3D12 probe: vkGetDeviceProcAddr unavailable; falling back");
        if (loaded_here) FreeLibrary(vulkan);
        return false;
    }

    const auto update = reinterpret_cast<PFN_vkAntiLagUpdateAMD>(
        get_device_proc_addr(caps.device, "vkAntiLagUpdateAMD"));
    if (!update) {
        spdlog::info(
            "Low-level D3D12 probe: vkAntiLagUpdateAMD not exposed on the vkd3d VkDevice; falling back");
        if (loaded_here) FreeLibrary(vulkan);
        return false;
    }

    vulkan_module_ = loaded_here ? vulkan : nullptr;
    vk_device_ = caps.device;
    anti_lag_update_ = update;
    last_input_frame_.store(INVALID_ID, std::memory_order_release);
    last_present_frame_.store(INVALID_ID, std::memory_order_release);
    paired_input_frame_.store(INVALID_ID, std::memory_order_release);
    pairing_active_.store(false, std::memory_order_release);
    requested_enabled_.store(false, std::memory_order_release);
    max_fps_.store(0, std::memory_order_release);
    override_mode_.store(ForceReflex::InGame, std::memory_order_release);
    hot_control_.store(0, std::memory_order_release);

    spdlog::info(
        "Low-level D3D12 backend: direct vkAntiLagUpdateAMD on vkd3d-proton's borrowed VkDevice");
    return true;
}

bool Vkd3dAntiLagVk::init_using_ctx(void*) {
    return false;
}

void Vkd3dAntiLagVk::deinit() {
    anti_lag_update_ = nullptr;
    vk_device_ = VK_NULL_HANDLE;
    if (vulkan_module_) {
        FreeLibrary(vulkan_module_);
        vulkan_module_ = nullptr;
    }
}

void Vkd3dAntiLagVk::rebuild_hot_control() noexcept {
    const auto override_mode = override_mode_.load(std::memory_order_acquire);
    const bool enabled = override_mode != ForceReflex::InGame
        ? override_mode == ForceReflex::ForceEnable
        : requested_enabled_.load(std::memory_order_acquire);
    const auto max_fps = max_fps_.load(std::memory_order_relaxed);
    hot_control_.store(
        static_cast<std::uint64_t>(max_fps) | (enabled ? kHotEnabled : 0),
        std::memory_order_release);
}

void Vkd3dAntiLagVk::set_low_latency_override(ForceReflex value) {
    override_mode_.store(value, std::memory_order_release);
    rebuild_hot_control();
}

void Vkd3dAntiLagVk::get_sleep_status(SleepParams* sleep_params) {
    if (!sleep_params) return;
    sleep_params->low_latency_enabled = is_enabled();
    sleep_params->fullscreen_vrr = true;
    sleep_params->control_panel_vsync_override = false;
}

void Vkd3dAntiLagVk::set_sleep_mode(SleepMode* sleep_mode) {
    if (!sleep_mode) return;
    requested_enabled_.store(sleep_mode->low_latency_enabled, std::memory_order_release);
    const auto interval = sleep_mode->minimum_interval_us;
    const std::uint32_t max_fps = interval > 0
        ? static_cast<std::uint32_t>((1'000'000ull + interval / 2u) / interval)
        : 0u;
    max_fps_.store(max_fps, std::memory_order_release);
    rebuild_hot_control();
}

void Vkd3dAntiLagVk::emit_input(std::uint64_t frame_id) noexcept {
    const auto update = anti_lag_update_;
    const auto device = vk_device_;
    if (!update || device == VK_NULL_HANDLE || frame_id == INVALID_ID) return;

    if (pairing_active_.load(std::memory_order_acquire)) {
        paired_input_frame_.store(frame_id, std::memory_order_release);
        emit_stage(VK_ANTI_LAG_STAGE_INPUT_AMD, frame_id);
        return;
    }

    // Before a matching Present marker has been proven, use the base Anti-Lag
    // form with no presentation info. This is explicitly valid and avoids
    // publishing an INPUT frameIndex that might never receive a matching
    // PRESENT stage. The first exact input/present match calibrates pairing for
    // subsequent frames.
    const auto hot_control = hot_control_.load(std::memory_order_acquire);
    VkAntiLagDataAMD data{};
    data.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
    data.mode = (hot_control & kHotEnabled) != 0 ? VK_ANTI_LAG_MODE_ON_AMD : VK_ANTI_LAG_MODE_OFF_AMD;
    data.maxFPS = static_cast<std::uint32_t>(hot_control);
    data.pPresentationInfo = nullptr;
    update(device, &data);
    paired_input_frame_.store(INVALID_ID, std::memory_order_release);
}

void Vkd3dAntiLagVk::emit_stage(VkAntiLagStageAMD stage, std::uint64_t frame_id) noexcept {
    const auto update = anti_lag_update_;
    const auto device = vk_device_;
    if (!update || device == VK_NULL_HANDLE || frame_id == INVALID_ID) return;

    VkAntiLagPresentationInfoAMD presentation{};
    presentation.sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD;
    presentation.stage = stage;
    presentation.frameIndex = frame_id;

    const auto hot_control = hot_control_.load(std::memory_order_acquire);
    VkAntiLagDataAMD data{};
    data.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
    data.mode = (hot_control & kHotEnabled) != 0 ? VK_ANTI_LAG_MODE_ON_AMD : VK_ANTI_LAG_MODE_OFF_AMD;
    data.maxFPS = static_cast<std::uint32_t>(hot_control);
    data.pPresentationInfo = &presentation;

    update(device, &data);
}


void Vkd3dAntiLagVk::sleep_with_frame_id(std::uint64_t frame_id) {
    // The pacing-owner Sleep callback is the earliest common cross-API anchor.
    // If an explicit INPUT_SAMPLE marker already arrived for this frame, do
    // nothing. Otherwise use the exact frozen pacing frame ID as the low-level
    // INPUT stage. This keeps one update per frame and avoids inventing a
    // simulation+1 frame domain.
    const auto previous = last_input_frame_.exchange(frame_id, std::memory_order_acq_rel);
    if (previous != frame_id) {
        emit_input(frame_id);
    }
}

void Vkd3dAntiLagVk::set_marker(IUnknown*, MarkerParams* marker_params) {
    if (!marker_params) return;

    switch (marker_params->marker_type) {
        case MarkerType::INPUT_SAMPLE: {
            const auto previous = last_input_frame_.exchange(
                marker_params->frame_id, std::memory_order_acq_rel);
            if (previous != marker_params->frame_id) {
                emit_input(marker_params->frame_id);
            }
            break;
        }
        case MarkerType::PRESENT_START: {
            const auto previous = last_present_frame_.exchange(
                marker_params->frame_id, std::memory_order_acq_rel);
            if (previous == marker_params->frame_id) break;

            if (paired_input_frame_.load(std::memory_order_acquire) == marker_params->frame_id) {
                emit_stage(VK_ANTI_LAG_STAGE_PRESENT_AMD, marker_params->frame_id);
            } else if (last_input_frame_.load(std::memory_order_acquire) == marker_params->frame_id) {
                // Exact canonical IDs proved that the frozen input/present
                // aspect sources share a frame domain. Enable paired staging
                // from the next input frame onward.
                pairing_active_.store(true, std::memory_order_release);
            }
            break;
        }
        default:
            break;
    }
}

void Vkd3dAntiLagVk::set_async_marker(MarkerParams* marker_params) {
    if (!marker_params) return;
    if (marker_params->marker_type != MarkerType::OUT_OF_BAND_PRESENT_START) return;

    const auto previous = last_present_frame_.exchange(
        marker_params->frame_id, std::memory_order_acq_rel);
    if (previous == marker_params->frame_id) return;

    if (paired_input_frame_.load(std::memory_order_acquire) == marker_params->frame_id) {
        emit_stage(VK_ANTI_LAG_STAGE_PRESENT_AMD, marker_params->frame_id);
    } else if (last_input_frame_.load(std::memory_order_acquire) == marker_params->frame_id) {
        pairing_active_.store(true, std::memory_order_release);
    }
}
