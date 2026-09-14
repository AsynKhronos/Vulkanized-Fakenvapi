#include "vkd3d_vulkan_capabilities.h"

#include "vkd3d_vulkan_interop.h"

#if _MSC_VER
#include <d3d12.h>
#else
#include "../external/d3d12.h"
#endif

#include <algorithm>
#include <array>
#include <cstring>

namespace {
constexpr std::size_t kMaxInteropExtensions = 512;

struct ExtensionBits {
    bool amd_anti_lag = false;
    bool nv_low_latency2 = false;
    bool timeline_semaphore = false;
    bool synchronization2 = false;
    bool present_id = false;
    bool present_id2 = false;
    bool present_wait = false;
    bool present_wait2 = false;
    bool present_timing = false;
    bool descriptor_buffer = false;
    bool device_fault = false;
    bool calibrated_timestamps = false;
};

void observe_extension(ExtensionBits& bits, const char* extension) noexcept {
    if (!extension) return;

    if (std::strcmp(extension, VK_AMD_ANTI_LAG_EXTENSION_NAME) == 0)
        bits.amd_anti_lag = true;
    else if (std::strcmp(extension, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0)
        bits.nv_low_latency2 = true;
    else if (std::strcmp(extension, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0)
        bits.timeline_semaphore = true;
    else if (std::strcmp(extension, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0)
        bits.synchronization2 = true;
    else if (std::strcmp(extension, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0)
        bits.present_id = true;
#ifdef VK_KHR_PRESENT_ID_2_EXTENSION_NAME
    else if (std::strcmp(extension, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) == 0)
        bits.present_id2 = true;
#endif
#ifdef VK_KHR_PRESENT_WAIT_EXTENSION_NAME
    else if (std::strcmp(extension, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0)
        bits.present_wait = true;
#endif
#ifdef VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME
    else if (std::strcmp(extension, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME) == 0)
        bits.present_wait2 = true;
#endif
#ifdef VK_EXT_PRESENT_TIMING_EXTENSION_NAME
    else if (std::strcmp(extension, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) == 0)
        bits.present_timing = true;
#endif
#ifdef VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME
    else if (std::strcmp(extension, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) == 0)
        bits.descriptor_buffer = true;
#endif
#ifdef VK_EXT_DEVICE_FAULT_EXTENSION_NAME
    else if (std::strcmp(extension, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) == 0)
        bits.device_fault = true;
#endif
#ifdef VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
    else if (std::strcmp(extension, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0)
        bits.calibrated_timestamps = true;
#endif
}

bool read_device_extensions(
    vkd3d_interop::ID3D12DXVKInteropDeviceMinimal* interop,
    ExtensionBits* out_bits,
    std::array<const char*, kMaxInteropExtensions>* out_extensions,
    UINT* out_count) noexcept {
    if (!interop || !out_bits || !out_extensions || !out_count) return false;
    *out_count = 0;

    UINT count = 0;
    if (FAILED(interop->GetDeviceExtensions(&count, nullptr)) || count > kMaxInteropExtensions)
        return false;

    if (count == 0)
        return true;

    UINT capacity = count;
    if (FAILED(interop->GetDeviceExtensions(&capacity, out_extensions->data())))
        return false;

    const UINT usable = std::min(count, capacity);
    *out_count = usable;
    for (UINT i = 0; i < usable; ++i)
        observe_extension(*out_bits, (*out_extensions)[i]);

    return true;
}

bool anti_lag_feature_enabled(
    vkd3d_interop::ID3D12DXVKInteropDeviceMinimal* interop) noexcept {
    if (!interop) return false;

    const VkPhysicalDeviceFeatures2* features = nullptr;
    if (FAILED(interop->GetDeviceFeatures(&features)) || !features)
        return false;

    const auto* node = reinterpret_cast<const VkBaseOutStructure*>(features);
    while (node) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD) {
            const auto* anti_lag = reinterpret_cast<const VkPhysicalDeviceAntiLagFeaturesAMD*>(node);
            return anti_lag->antiLag == VK_TRUE;
        }
        node = node->pNext;
    }
    return false;
}


void read_present_features(
    vkd3d_interop::ID3D12DXVKInteropDeviceMinimal* interop,
    Vkd3dVulkanCapabilities* caps) noexcept {
    if (!interop || !caps) return;

    const VkPhysicalDeviceFeatures2* features = nullptr;
    if (FAILED(interop->GetDeviceFeatures(&features)) || !features)
        return;

    const auto* node = reinterpret_cast<const VkBaseOutStructure*>(features);
    while (node) {
        switch (node->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR:
                caps->present_id_feature =
                    reinterpret_cast<const VkPhysicalDevicePresentIdFeaturesKHR*>(node)->presentId == VK_TRUE;
                break;
#ifdef VK_KHR_present_id2
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR:
                caps->present_id2_feature =
                    reinterpret_cast<const VkPhysicalDevicePresentId2FeaturesKHR*>(node)->presentId2 == VK_TRUE;
                break;
#endif
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR:
                caps->present_wait_feature =
                    reinterpret_cast<const VkPhysicalDevicePresentWaitFeaturesKHR*>(node)->presentWait == VK_TRUE;
                break;
#ifdef VK_KHR_present_wait2
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR:
                caps->present_wait2_feature =
                    reinterpret_cast<const VkPhysicalDevicePresentWait2FeaturesKHR*>(node)->presentWait2 == VK_TRUE;
                break;
#endif
#ifdef VK_EXT_present_timing
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT:
                caps->present_timing_feature =
                    reinterpret_cast<const VkPhysicalDevicePresentTimingFeaturesEXT*>(node)->presentTiming == VK_TRUE;
                break;
#endif
            default:
                break;
        }
        node = node->pNext;
    }
}

void query_physical_device_metadata_and_capabilities(
    Vkd3dVulkanCapabilities& caps,
    std::span<const char* const> enabled_extensions,
    const VkPhysicalDeviceFeatures2* enabled_features) noexcept {
    if (caps.instance == VK_NULL_HANDLE || caps.physical_device == VK_NULL_HANDLE)
        return;

    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    if (!vulkan)
        return;

    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
    if (!gipa)
        return;

    const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        gipa(caps.instance, "vkGetPhysicalDeviceProperties"));
    auto get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        gipa(caps.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!get_features2) {
        get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            gipa(caps.instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    const auto enumerate_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        gipa(caps.instance, "vkEnumerateDeviceExtensionProperties"));

    const VulkanCapabilityProbeDispatch dispatch{get_properties, get_features2, enumerate_extensions};
    if (probe_vulkan_capability_support(caps.physical_device, dispatch, &caps.capabilities)) {
        observe_vulkan_enabled_state(enabled_extensions, enabled_features, &caps.capabilities);
        caps.api_version = caps.capabilities.api_version;
        caps.driver_version = caps.capabilities.driver_version;
        caps.vendor_id = caps.capabilities.vendor_id;
        caps.device_id = caps.capabilities.device_id;
    }

    const auto get_queue_families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa(caps.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!get_queue_families)
        return;

    std::uint32_t count = 0;
    get_queue_families(caps.physical_device, &count, nullptr);
    caps.queue_family_count = count;
    if (count == 0)
        return;

    constexpr std::uint32_t kMaxQueueFamilies = 32;
    std::array<VkQueueFamilyProperties, kMaxQueueFamilies> properties{};
    std::uint32_t capacity = std::min(count, kMaxQueueFamilies);
    get_queue_families(caps.physical_device, &capacity, properties.data());
    caps.queue_family_count = capacity;

    for (std::uint32_t i = 0; i < capacity; ++i) {
        const VkQueueFlags flags = properties[i].queueFlags;
        caps.graphics_queue_families += (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
        caps.compute_queue_families += (flags & VK_QUEUE_COMPUTE_BIT) != 0;
        caps.transfer_queue_families += (flags & VK_QUEUE_TRANSFER_BIT) != 0;
    }
}
} // namespace

bool probe_vkd3d_vulkan_capabilities(
    IUnknown* d3d_device,
    Vkd3dVulkanCapabilities* out_caps) noexcept {
    if (!d3d_device || !out_caps)
        return false;

    *out_caps = {};

    ID3D12Device* d3d12 = nullptr;
    if (FAILED(d3d_device->QueryInterface(
            __uuidof(ID3D12Device), reinterpret_cast<void**>(&d3d12))) || !d3d12) {
        return false;
    }

    vkd3d_interop::ID3D12DXVKInteropDeviceMinimal* interop = nullptr;
    const HRESULT interop_hr = d3d12->QueryInterface(
        vkd3d_interop::IID_ID3D12DXVKInteropDevice_Value,
        reinterpret_cast<void**>(&interop));
    if (FAILED(interop_hr) || !interop) {
        d3d12->Release();
        return false;
    }

    out_caps->interop = true;

    // Device2 is optional. Querying it is a version/capability probe only; the
    // backend deliberately relies on the stable base interface for execution.
    IUnknown* interop_v2 = nullptr;
    if (SUCCEEDED(d3d12->QueryInterface(
            vkd3d_interop::IID_ID3D12DXVKInteropDevice2_Value,
            reinterpret_cast<void**>(&interop_v2))) && interop_v2) {
        out_caps->interop_v2 = true;
        interop_v2->Release();
    }
    d3d12->Release();

    ExtensionBits bits{};
    std::array<const char*, kMaxInteropExtensions> enabled_extensions{};
    UINT enabled_extension_count = 0;
    (void)read_device_extensions(interop, &bits, &enabled_extensions, &enabled_extension_count);
    out_caps->amd_anti_lag_extension = bits.amd_anti_lag;
    out_caps->nv_low_latency2_extension = bits.nv_low_latency2;
    out_caps->timeline_semaphore = bits.timeline_semaphore;
    out_caps->synchronization2 = bits.synchronization2;
    out_caps->present_id = bits.present_id;
    out_caps->present_id2 = bits.present_id2;
    out_caps->present_wait = bits.present_wait;
    out_caps->present_wait2 = bits.present_wait2;
    out_caps->present_timing = bits.present_timing;
    const VkPhysicalDeviceFeatures2* enabled_features = nullptr;
    (void)interop->GetDeviceFeatures(&enabled_features);
    read_present_features(interop, out_caps);
    out_caps->descriptor_buffer = bits.descriptor_buffer;
    out_caps->device_fault = bits.device_fault;
    out_caps->calibrated_timestamps = bits.calibrated_timestamps;
    out_caps->amd_anti_lag_feature =
        out_caps->amd_anti_lag_extension && anti_lag_feature_enabled(interop);

    const HRESULT handles_hr = interop->GetVulkanHandles(
        &out_caps->instance,
        &out_caps->physical_device,
        &out_caps->device);

    if (FAILED(handles_hr) || out_caps->device == VK_NULL_HANDLE) {
        interop->Release();
        out_caps->instance = VK_NULL_HANDLE;
        out_caps->physical_device = VK_NULL_HANDLE;
        out_caps->device = VK_NULL_HANDLE;
        return true;
    }

    query_physical_device_metadata_and_capabilities(
        *out_caps,
        std::span<const char* const>(enabled_extensions.data(), enabled_extension_count),
        enabled_features);
    interop->Release();

    // Account for extensions promoted to core. vkd3d-proton may omit the
    // extension name when the same primitive is provided by the device's core
    // Vulkan version.
    if (out_caps->capabilities.supports(VulkanCapability::TimelineSemaphore))
        out_caps->timeline_semaphore = true;
    if (out_caps->capabilities.supports(VulkanCapability::Synchronization2))
        out_caps->synchronization2 = true;

    return true;
}
