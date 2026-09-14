#pragma once

#include "vulkan_capabilities.h"

#include <vulkan/vulkan_core.h>

#include <cstdint>

struct VulkanDeviceState {
    VkDevice device = VK_NULL_HANDLE;

    PFN_vkCreateSemaphore create_semaphore = nullptr;
    PFN_vkDestroySemaphore destroy_semaphore = nullptr;
    PFN_vkSignalSemaphore signal_semaphore = nullptr;
    PFN_vkAntiLagUpdateAMD anti_lag_update = nullptr;
    PFN_vkGetDeviceQueue get_device_queue = nullptr;
    PFN_vkGetDeviceQueue2 get_device_queue2 = nullptr;
    PFN_vkAcquireNextImageKHR acquire_next_image = nullptr;
    PFN_vkAcquireNextImage2KHR acquire_next_image2 = nullptr;
    PFN_vkCreateSwapchainKHR create_swapchain = nullptr;
    PFN_vkQueuePresentKHR queue_present = nullptr;
    PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
    // R4.0 rare-path pipeline creation observation. These retained real entry
    // points let the hook time the driver's call without rewriting create infos.
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines = nullptr;
    PFN_vkCreateComputePipelines create_compute_pipelines = nullptr;

    // R3.6 present-precision dispatch. These are passive capability consumers:
    // VulkanFlex does not enable extensions/features on behalf of the game.
    PFN_vkWaitForPresentKHR wait_for_present = nullptr;
    PFN_vkWaitForPresent2KHR wait_for_present2 = nullptr;
    PFN_vkGetPastPresentationTimingEXT get_past_presentation_timing = nullptr;

    // Native VK_NV_low_latency2 entry points are retained only when the
    // underlying Vulkan implementation really supports the extension and the
    // policy chooses native pass-through. Emulated devices leave these null.
    PFN_vkSetLatencySleepModeNV nv_set_latency_sleep_mode = nullptr;
    PFN_vkLatencySleepNV nv_latency_sleep = nullptr;
    PFN_vkSetLatencyMarkerNV nv_set_latency_marker = nullptr;
    PFN_vkGetLatencyTimingsNV nv_get_latency_timings = nullptr;
    PFN_vkQueueNotifyOutOfBandNV nv_queue_notify_out_of_band = nullptr;

    VkSemaphore low_latency_semaphore = VK_NULL_HANDLE;
    bool amd_anti_lag_enabled = false;
    bool nv_low_latency2_enabled = false;
    bool nv_low_latency2_native = false;
    bool present_id_enabled = false;
    bool present_id2_enabled = false;
    bool present_wait_enabled = false;
    bool present_wait2_enabled = false;
    bool present_timing_enabled = false;

    // R3.9 transport-neutral capability snapshot. Immutable after registration.
    VulkanCapabilitySnapshot capabilities{};
};

// Compact per-call dispatch view for VK_NV_low_latency2. This exists so the
// frame path does not copy the entire VulkanDeviceState merely to read one or
// two function pointers.

struct VulkanPresentPrecisionDispatch {
    PFN_vkWaitForPresentKHR wait_for_present = nullptr;
    PFN_vkWaitForPresent2KHR wait_for_present2 = nullptr;
    PFN_vkGetPastPresentationTimingEXT get_past_presentation_timing = nullptr;
    bool present_id_enabled = false;
    bool present_id2_enabled = false;
    bool present_wait_enabled = false;
    bool present_wait2_enabled = false;
    bool present_timing_enabled = false;
};

struct VulkanSwapchainState {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainCreateFlagsKHR flags = 0;
};
struct VulkanNvLowLatency2Dispatch {
    PFN_vkSetLatencySleepModeNV set_sleep_mode = nullptr;
    PFN_vkLatencySleepNV latency_sleep = nullptr;
    PFN_vkSetLatencyMarkerNV set_marker = nullptr;
    PFN_vkGetLatencyTimingsNV get_timings = nullptr;
    bool enabled = false;
    bool native = false;
};

class VulkanDeviceRegistry {
public:
    static bool register_device(const VulkanDeviceState& state);
    static void unregister_device(VkDevice device);

    // Read-mostly registry API. Readers never take the writer mutex. Device
    // creation/destruction and queue discovery publish a new generation, while
    // frame-hot lookups hit thread-local snapshots after the first observation.
    static bool get_state(VkDevice device, VulkanDeviceState* out_state);
    // Frame-hot WSI dispatch accessors. These return only the retained function
    // pointer needed by the hook instead of copying the full device snapshot.
    static PFN_vkAcquireNextImageKHR get_acquire_next_image(VkDevice device);
    static PFN_vkAcquireNextImage2KHR get_acquire_next_image2(VkDevice device);
    static bool get_queue_present_dispatch(
        VkQueue queue, VkDevice* out_device, PFN_vkQueuePresentKHR* out_present);
    static bool get_nv_low_latency2_dispatch(
        VkDevice device,
        VulkanNvLowLatency2Dispatch* out_dispatch);
    static bool get_present_precision_dispatch(
        VkDevice device,
        VulkanPresentPrecisionDispatch* out_dispatch);
    static bool get_capabilities(
        VkDevice device,
        VulkanCapabilitySnapshot* out_capabilities);

    static bool has_amd_anti_lag(VkDevice device);
    static bool has_native_nv_low_latency2(VkDevice device);
    static PFN_vkAntiLagUpdateAMD get_anti_lag_update(VkDevice device);

    static bool register_queue(VkDevice device, VkQueue queue);
    static bool get_device_for_queue(VkQueue queue, VkDevice* out_device);

    static bool register_swapchain(
        VkDevice device, VkSwapchainKHR swapchain, VkSwapchainCreateFlagsKHR flags);
    static void unregister_swapchain(VkSwapchainKHR swapchain);
    static bool get_swapchain_state(VkSwapchainKHR swapchain, VulkanSwapchainState* out_state);
    static PFN_vkQueueNotifyOutOfBandNV get_native_queue_notify(VkQueue queue);

    static VkResult ensure_low_latency_semaphore(VkDevice device, VkSemaphore* out_semaphore);
    static VkResult signal_low_latency_semaphore(VkDevice device, uint64_t value);
    static VkResult signal_semaphore(VkDevice device, VkSemaphore semaphore, uint64_t value);
    static void destroy_low_latency_semaphore(VkDevice device);
};
