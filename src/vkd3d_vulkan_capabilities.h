#pragma once

#include "vulkan_capabilities.h"

#include <windows.h>
#include <unknwn.h>
#include <vulkan/vulkan_core.h>

#include <cstdint>

struct Vkd3dVulkanCapabilities {
    bool interop = false;
    bool interop_v2 = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;

    std::uint32_t queue_family_count = 0;
    std::uint32_t graphics_queue_families = 0;
    std::uint32_t compute_queue_families = 0;
    std::uint32_t transfer_queue_families = 0;

    bool amd_anti_lag_extension = false;
    bool amd_anti_lag_feature = false;
    bool nv_low_latency2_extension = false;
    bool timeline_semaphore = false;
    bool synchronization2 = false;
    bool present_id = false;
    bool present_id_feature = false;
    bool present_id2 = false;
    bool present_id2_feature = false;
    bool present_wait = false;
    bool present_wait_feature = false;
    bool present_wait2 = false;
    bool present_wait2_feature = false;
    bool present_timing = false;
    bool present_timing_feature = false;
    bool descriptor_buffer = false;
    bool device_fault = false;
    bool calibrated_timestamps = false;

    // R3.9 shared transport-neutral Vulkan feature model.
    VulkanCapabilitySnapshot capabilities{};
};

// Probe the concrete D3D12 device through vkd3d-proton's public Vulkan interop
// contract. This is initialization/control-plane work only; no result here is
// intended to be queried from a frame hot path.
[[nodiscard]] bool probe_vkd3d_vulkan_capabilities(
    IUnknown* d3d_device,
    Vkd3dVulkanCapabilities* out_caps) noexcept;
