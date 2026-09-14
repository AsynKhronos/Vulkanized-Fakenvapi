#pragma once

// Minimal public vkd3d-proton interop ABI used by Vulkanized-Fakenvapi.
// The IID and method order mirror ID3D12DXVKInteropDevice from
// vkd3d_device_vkd3d_ext.idl. Keeping the declaration local avoids adding a
// build dependency on vkd3d-proton headers while still using its documented
// COM interop contract when present.

#include <windows.h>
#include <unknwn.h>
#if _MSC_VER
#include <d3d12.h>
#else
#include "../external/d3d12.h"
#endif
#include <vulkan/vulkan_core.h>

namespace vkd3d_interop {

inline constexpr GUID IID_ID3D12DXVKInteropDevice_Value = {
    0x39da4e09, 0xbd1c, 0x4198, {0x9f, 0xae, 0x86, 0xbb, 0xe3, 0xbe, 0x41, 0xfd}
};

// Current vkd3d-proton/Proton exposes Device2 as an optional superset. We only
// query this IID as a capability/version signal; execution stays on the stable
// base ABI below.
inline constexpr GUID IID_ID3D12DXVKInteropDevice2_Value = {
    0x90ecf26e, 0xb212, 0x43f5, {0xb6, 0x2a, 0x82, 0x5a, 0xd7, 0xb1, 0x38, 0x5e}
};

struct ID3D12DXVKInteropDeviceMinimal : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID iid, void** object) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT* extension_count, const char** extensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT* extension_count, const char** extensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2** features) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(
        VkInstance* vk_instance,
        VkPhysicalDevice* vk_physical_device,
        VkDevice* vk_device) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(
        ID3D12CommandQueue* queue,
        VkQueue* vk_queue,
        UINT32* vk_queue_family) = 0;
    virtual void STDMETHODCALLTYPE GetVulkanImageLayout(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES state,
        VkImageLayout* vk_layout) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo(
        ID3D12Resource* resource,
        UINT64* vk_handle,
        UINT64* buffer_offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockCommandQueue(ID3D12CommandQueue* queue) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockCommandQueue(ID3D12CommandQueue* queue) = 0;
};

} // namespace vkd3d_interop
