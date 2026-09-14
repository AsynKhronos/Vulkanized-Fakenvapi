#include "dxvk_vulkan_interop.h"

#include <windows.h>

namespace dxvk_interop {

bool probe_dxvk_vulkan_capabilities(
    IUnknown* device,
    DxvkVulkanCapabilities* capabilities) noexcept {
    if (!capabilities) return false;
    *capabilities = {};
    if (!device) return false;

    IDXGIVkInteropDeviceMinimal* interop = nullptr;
    const HRESULT interop_hr = device->QueryInterface(
        IID_IDXGIVkInteropDevice_Value,
        reinterpret_cast<void**>(&interop));
    if (FAILED(interop_hr) || !interop)
        return false;

    interop->GetVulkanHandles(
        &capabilities->instance,
        &capabilities->physical_device,
        &capabilities->device);
    interop->GetSubmissionQueue(
        &capabilities->queue,
        &capabilities->queue_family);
    capabilities->interop = capabilities->device != VK_NULL_HANDLE;
    interop->Release();

    // Public DXVK interop gives us physical-device identity but intentionally
    // not DXVK's private VkDeviceCreateInfo. Probe support through public Vulkan
    // and leave non-core enabled state unknown.
    if (capabilities->interop && capabilities->instance != VK_NULL_HANDLE &&
        capabilities->physical_device != VK_NULL_HANDLE) {
        HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
        if (vulkan) {
            const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
            if (gipa) {
                const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                    gipa(capabilities->instance, "vkGetPhysicalDeviceProperties"));
                auto get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                    gipa(capabilities->instance, "vkGetPhysicalDeviceFeatures2"));
                if (!get_features2) {
                    get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                        gipa(capabilities->instance, "vkGetPhysicalDeviceFeatures2KHR"));
                }
                const auto enumerate_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                    gipa(capabilities->instance, "vkEnumerateDeviceExtensionProperties"));
                const VulkanCapabilityProbeDispatch dispatch{
                    get_properties, get_features2, enumerate_extensions};
                (void)probe_vulkan_capability_support(
                    capabilities->physical_device, dispatch, &capabilities->capabilities);
            }
        }
    }

    ID3DLowLatencyDeviceMinimal* low_latency = nullptr;
    const HRESULT ll_hr = device->QueryInterface(
        IID_ID3DLowLatencyDevice_Value,
        reinterpret_cast<void**>(&low_latency));
    if (SUCCEEDED(ll_hr) && low_latency) {
        capabilities->low_latency_interface = true;
        capabilities->low_latency_supported = low_latency->SupportsLowLatency() != FALSE;
        low_latency->Release();
    }

    return capabilities->interop;
}

} // namespace dxvk_interop
