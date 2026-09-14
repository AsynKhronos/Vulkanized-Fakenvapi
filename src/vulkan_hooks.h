#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>
#include <vulkan/vulkan_core.h>
#include <cstdint>

class VulkanHooks {
private:
    static PFN_vkGetInstanceProcAddr o_vkGetInstanceProcAddr;
    static PFN_vkGetDeviceProcAddr o_vkGetDeviceProcAddr;
    static PFN_vkCreateDevice o_vkCreateDevice;
    static PFN_vkDestroyDevice o_vkDestroyDevice;
    static PFN_vkGetPhysicalDeviceFeatures2 o_vkGetPhysicalDeviceFeatures2;
    static PFN_vkGetPhysicalDeviceProperties o_vkGetPhysicalDeviceProperties;
    static PFN_vkEnumerateDeviceExtensionProperties o_vkEnumerateDeviceExtensionProperties;
    static PFN_vkGetDeviceQueue o_vkGetDeviceQueue;
    static PFN_vkGetDeviceQueue2 o_vkGetDeviceQueue2;
    static PFN_vkAcquireNextImageKHR o_vkAcquireNextImageKHR;
    static PFN_vkAcquireNextImage2KHR o_vkAcquireNextImage2KHR;
    static PFN_vkCreateSwapchainKHR o_vkCreateSwapchainKHR;
    static PFN_vkQueuePresentKHR o_vkQueuePresentKHR;
    static PFN_vkDestroySwapchainKHR o_vkDestroySwapchainKHR;

public:
    // Installs Vulkan hooks immediately when vulkan-1.dll is already loaded, or
    // installs a lightweight LoadLibrary watcher so a later Vulkan loader is
    // intercepted before the caller can resolve device extension entry points.
    static void initialize(HMODULE vulkan_module = nullptr);
    static void shutdown();
    static void hook_vulkan(HMODULE vulkan_module);

    static VkResult VKAPI_CALL hkvkCreateDevice(
        VkPhysicalDevice physical_device,
        const VkDeviceCreateInfo* create_info,
        const VkAllocationCallbacks* allocator,
        VkDevice* device);

    static void VKAPI_CALL hkvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator);

    static VkResult VKAPI_CALL hkvkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice physical_device,
        const char* layer_name,
        uint32_t* property_count,
        VkExtensionProperties* properties);

    static PFN_vkVoidFunction VKAPI_CALL hkvkGetDeviceProcAddr(VkDevice device, const char* name);
    static PFN_vkVoidFunction VKAPI_CALL hkvkGetInstanceProcAddr(VkInstance instance, const char* name);

    static void VKAPI_CALL hkvkGetDeviceQueue(
        VkDevice device,
        uint32_t queue_family_index,
        uint32_t queue_index,
        VkQueue* queue);

    static void VKAPI_CALL hkvkGetDeviceQueue2(
        VkDevice device,
        const VkDeviceQueueInfo2* queue_info,
        VkQueue* queue);

    static VkResult VKAPI_CALL hkvkAcquireNextImageKHR(
        VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
        VkSemaphore semaphore, VkFence fence, uint32_t* image_index);

    static VkResult VKAPI_CALL hkvkAcquireNextImage2KHR(
        VkDevice device, const VkAcquireNextImageInfoKHR* acquire_info, uint32_t* image_index);

    static VkResult VKAPI_CALL hkvkCreateSwapchainKHR(
        VkDevice device, const VkSwapchainCreateInfoKHR* create_info,
        const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain);

    static VkResult VKAPI_CALL hkvkCreateGraphicsPipelines(
        VkDevice device, VkPipelineCache pipeline_cache, std::uint32_t create_info_count,
        const VkGraphicsPipelineCreateInfo* create_infos,
        const VkAllocationCallbacks* allocator, VkPipeline* pipelines);

    static VkResult VKAPI_CALL hkvkCreateComputePipelines(
        VkDevice device, VkPipelineCache pipeline_cache, std::uint32_t create_info_count,
        const VkComputePipelineCreateInfo* create_infos,
        const VkAllocationCallbacks* allocator, VkPipeline* pipelines);

    static VkResult VKAPI_CALL hkvkQueuePresentKHR(
        VkQueue queue, const VkPresentInfoKHR* present_info);

    static void VKAPI_CALL hkvkDestroySwapchainKHR(
        VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator);

    // VK_NV_low_latency2 frontend. These functions are returned from
    // vkGetDeviceProcAddr/vkGetInstanceProcAddr when the extension is native or
    // emulated. Native support can be preserved as the sole output; otherwise
    // calls are translated into the common Vulkan low-latency backend.
    static VkResult VKAPI_CALL hkvkSetLatencySleepModeNV(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkLatencySleepModeInfoNV* sleep_mode_info);

    static VkResult VKAPI_CALL hkvkLatencySleepNV(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkLatencySleepInfoNV* sleep_info);

    static void VKAPI_CALL hkvkSetLatencyMarkerNV(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkSetLatencyMarkerInfoNV* marker_info);

    static void VKAPI_CALL hkvkGetLatencyTimingsNV(
        VkDevice device,
        VkSwapchainKHR swapchain,
        VkGetLatencyMarkerInfoNV* marker_info);

    static void VKAPI_CALL hkvkQueueNotifyOutOfBandNV(
        VkQueue queue,
        const VkOutOfBandQueueTypeInfoNV* queue_type_info);
};
