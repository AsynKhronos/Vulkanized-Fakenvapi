#pragma once

// Minimal public DXVK COM interop ABI used by Vulkanized-Fakenvapi.
// IIDs and method order mirror DXVK's public dxgi_interfaces.h and
// d3d11_interfaces.h.  We intentionally use only the read/control-plane
// prefix: Vulkan handles, submission-queue identity and DXVK's low-latency
// interface.  VulkanFlex never locks the DXVK queue or submits Vulkan work.

#include "vulkan_capabilities.h"

#include <windows.h>
#include <unknwn.h>
#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace dxvk_interop {

inline constexpr GUID IID_IDXGIVkInteropDevice_Value = {
    0xe2ef5fa5, 0xdc21, 0x4af7, {0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x23}
};

inline constexpr GUID IID_ID3DLowLatencyDevice_Value = {
    0xf3112584, 0x41f9, 0x348d, {0xa5, 0x9b, 0x00, 0xb7, 0xe1, 0xd2, 0x85, 0xd6}
};

struct IDXGIVkInteropDeviceMinimal : IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(
        VkInstance* instance,
        VkPhysicalDevice* physical_device,
        VkDevice* device) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(
        VkQueue* queue,
        std::uint32_t* queue_family_index) = 0;
};

struct ID3DLowLatencyDeviceMinimal : IUnknown {
    virtual BOOL STDMETHODCALLTYPE SupportsLowLatency() = 0;
    virtual HRESULT STDMETHODCALLTYPE LatencySleep() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLatencySleepMode(
        BOOL low_latency_enable,
        BOOL low_latency_boost,
        UINT32 minimum_interval_us) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLatencyMarker(
        UINT64 frame_id,
        UINT32 marker_type) = 0;
};

struct DxvkVulkanCapabilities {
    bool interop = false;
    bool low_latency_interface = false;
    bool low_latency_supported = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0xffffffffu;
    // DXVK public interop exposes the underlying physical device but not the
    // concrete VkDeviceCreateInfo. Therefore R3.9 records physical support and
    // leaves most enabled-state bits unknown rather than guessing.
    VulkanCapabilitySnapshot capabilities{};
};

[[nodiscard]] bool probe_dxvk_vulkan_capabilities(
    IUnknown* device,
    DxvkVulkanCapabilities* capabilities) noexcept;

} // namespace dxvk_interop
