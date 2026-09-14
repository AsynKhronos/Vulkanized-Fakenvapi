#include "vulkan_capabilities.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
std::uint32_t g_api_version = VK_API_VERSION_1_4;

constexpr const char* kExtensions[] = {
    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME,
    VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME,
    VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME,
    VK_KHR_PRESENT_ID_EXTENSION_NAME,
    VK_KHR_PRESENT_ID_2_EXTENSION_NAME,
    VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
    VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME,
    VK_EXT_PRESENT_TIMING_EXTENSION_NAME,
    VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
};

VKAPI_ATTR void VKAPI_CALL fake_get_properties(
    VkPhysicalDevice, VkPhysicalDeviceProperties* properties) {
    *properties = {};
    properties->apiVersion = g_api_version;
    properties->driverVersion = 123;
    properties->vendorID = 0x1002;
    properties->deviceID = 0x7550;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_enumerate_extensions(
    VkPhysicalDevice,
    const char*,
    std::uint32_t* count,
    VkExtensionProperties* properties) {
    assert(count);
    const auto total = static_cast<std::uint32_t>(std::size(kExtensions));
    if (!properties) {
        *count = total;
        return VK_SUCCESS;
    }

    const auto write = *count < total ? *count : total;
    for (std::uint32_t i = 0; i < write; ++i) {
        properties[i] = {};
        std::strncpy(properties[i].extensionName, kExtensions[i], VK_MAX_EXTENSION_NAME_SIZE - 1);
        properties[i].specVersion = 1;
    }
    *count = write;
    return write == total ? VK_SUCCESS : VK_INCOMPLETE;
}

VKAPI_ATTR void VKAPI_CALL fake_get_features2(
    VkPhysicalDevice, VkPhysicalDeviceFeatures2* features) {
    assert(features);
    auto* node = reinterpret_cast<VkBaseOutStructure*>(features->pNext);
    while (node) {
        switch (node->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
                reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(node)->timelineSemaphore = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
                auto* f = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(node);
                f->synchronization2 = VK_TRUE;
                f->dynamicRendering = VK_TRUE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES: {
                auto* f = reinterpret_cast<VkPhysicalDeviceVulkan14Features*>(node);
                f->dynamicRenderingLocalRead = VK_TRUE;
                f->maintenance5 = VK_TRUE;
                f->maintenance6 = VK_TRUE;
                f->pipelineRobustness = VK_TRUE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
                reinterpret_cast<VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*>(node)->extendedDynamicState = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT: {
                auto* f = reinterpret_cast<VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*>(node);
                f->extendedDynamicState2 = VK_TRUE;
                f->extendedDynamicState2LogicOp = VK_TRUE;
                f->extendedDynamicState2PatchControlPoints = VK_FALSE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT: {
                auto* f = reinterpret_cast<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT*>(node);
                f->extendedDynamicState3PolygonMode = VK_TRUE;
                f->extendedDynamicState3RasterizationSamples = VK_TRUE;
                f->extendedDynamicState3ColorBlendEnable = VK_TRUE;
                f->extendedDynamicState3ColorBlendEquation = VK_TRUE;
                f->extendedDynamicState3ColorWriteMask = VK_TRUE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
                reinterpret_cast<VkPhysicalDeviceDescriptorBufferFeaturesEXT*>(node)->descriptorBuffer = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT:
                reinterpret_cast<VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT*>(node)->graphicsPipelineLibrary = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT:
                reinterpret_cast<VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT*>(node)->shaderModuleIdentifier = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR:
                reinterpret_cast<VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR*>(node)->unifiedImageLayouts = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR:
                reinterpret_cast<VkPhysicalDevicePresentIdFeaturesKHR*>(node)->presentId = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR:
                reinterpret_cast<VkPhysicalDevicePresentId2FeaturesKHR*>(node)->presentId2 = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR:
                reinterpret_cast<VkPhysicalDevicePresentWaitFeaturesKHR*>(node)->presentWait = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR:
                reinterpret_cast<VkPhysicalDevicePresentWait2FeaturesKHR*>(node)->presentWait2 = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT:
                reinterpret_cast<VkPhysicalDevicePresentTimingFeaturesEXT*>(node)->presentTiming = VK_TRUE;
                break;
            default:
                break;
        }
        node = node->pNext;
    }
}

template <typename T, typename U>
void chain(T& a, U& b) {
    a.pNext = &b;
}

} // namespace

int main() {
    VulkanCapabilitySnapshot support{};
    const VulkanCapabilityProbeDispatch dispatch{
        fake_get_properties,
        fake_get_features2,
        fake_enumerate_extensions,
    };
    const auto physical = reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(0x1));
    assert(probe_vulkan_capability_support(physical, dispatch, &support));
    assert(support.support_probe_complete);
    assert(support.api_version == VK_API_VERSION_1_4);
    assert(support.supports(VulkanCapability::Vulkan13));
    assert(support.supports(VulkanCapability::Vulkan14));
    assert(support.supports(VulkanCapability::TimelineSemaphore));
    assert(support.supports(VulkanCapability::Synchronization2));
    assert(support.supports(VulkanCapability::DynamicRendering));
    assert(support.supports(VulkanCapability::DynamicRenderingLocalRead));
    assert(support.supports(VulkanCapability::ExtendedDynamicState1));
    assert(support.supports(VulkanCapability::ExtendedDynamicState2));
    assert(support.supports(VulkanCapability::ExtendedDynamicState2LogicOp));
    assert(!support.supports(VulkanCapability::ExtendedDynamicState2PatchControlPoints));
    assert(support.supports(VulkanCapability::ExtendedDynamicState3));
    assert(vulkan_eds3_feature_count(support.eds3_supported) == 5);
    assert(support.supports_eds3(VulkanEds3Feature::PolygonMode));
    assert(support.supports_eds3(VulkanEds3Feature::ColorBlendEquation));
    assert(!support.supports_eds3(VulkanEds3Feature::ViewportSwizzle));
    assert(support.supports(VulkanCapability::DescriptorBuffer));
    assert(support.supports(VulkanCapability::GraphicsPipelineLibrary));
    assert(support.supports(VulkanCapability::ShaderModuleIdentifier));
    assert(support.supports(VulkanCapability::UnifiedImageLayouts));
    assert(support.supports(VulkanCapability::Maintenance5));
    assert(support.supports(VulkanCapability::Maintenance6));
    assert(support.supports(VulkanCapability::PipelineRobustness));
    assert(support.supports(VulkanCapability::PresentId));
    assert(support.supports(VulkanCapability::PresentId2));
    assert(support.supports(VulkanCapability::PresentWait));
    assert(support.supports(VulkanCapability::PresentWait2));
    assert(support.supports(VulkanCapability::PresentTiming));

    // A support-only probe must not pretend optional VkDevice features are enabled.
    assert(!support.knows_enabled(VulkanCapability::DescriptorBuffer));
    assert(!support.knows_enabled(VulkanCapability::ExtendedDynamicState3));
    assert(support.is_enabled(VulkanCapability::ExtendedDynamicState1));
    assert(support.is_enabled(VulkanCapability::ExtendedDynamicState2));

    VkPhysicalDeviceVulkan12Features e12{};
    e12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    e12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan13Features e13{};
    e13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    e13.synchronization2 = VK_TRUE;
    e13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan14Features e14{};
    e14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    e14.dynamicRenderingLocalRead = VK_TRUE;
    e14.maintenance5 = VK_TRUE;
    e14.maintenance6 = VK_TRUE;
    e14.pipelineRobustness = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT e2{};
    e2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
    e2.extendedDynamicState2 = VK_TRUE;
    e2.extendedDynamicState2LogicOp = VK_TRUE;
    e2.extendedDynamicState2PatchControlPoints = VK_FALSE;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3{};
    e3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    e3.extendedDynamicState3PolygonMode = VK_TRUE;
    e3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
    e3.extendedDynamicState3ColorWriteMask = VK_TRUE;
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor{};
    descriptor.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    descriptor.descriptorBuffer = VK_TRUE;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{};
    gpl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
    gpl.graphicsPipelineLibrary = VK_FALSE;
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT shader_id{};
    shader_id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT;
    shader_id.shaderModuleIdentifier = VK_TRUE;
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unified{};
    unified.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR;
    unified.unifiedImageLayouts = VK_TRUE;
    VkPhysicalDevicePresentIdFeaturesKHR present_id{};
    present_id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    present_id.presentId = VK_TRUE;
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait{};
    present_wait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    present_wait.presentWait = VK_TRUE;

    chain(e12, e13);
    chain(e13, e14);
    chain(e14, e2);
    chain(e2, e3);
    chain(e3, descriptor);
    chain(descriptor, gpl);
    chain(gpl, shader_id);
    chain(shader_id, unified);
    chain(unified, present_id);
    chain(present_id, present_wait);

    const char* enabled_extensions[] = {
        VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME,
        VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME,
        VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME,
        VK_KHR_PRESENT_ID_EXTENSION_NAME,
        VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
    };
    VkDeviceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.pNext = &e12;
    create.enabledExtensionCount = static_cast<std::uint32_t>(std::size(enabled_extensions));
    create.ppEnabledExtensionNames = enabled_extensions;
    observe_vulkan_device_create_info(create, &support);

    assert(support.is_enabled(VulkanCapability::TimelineSemaphore));
    assert(support.is_enabled(VulkanCapability::Synchronization2));
    assert(support.is_enabled(VulkanCapability::DynamicRendering));
    assert(support.is_enabled(VulkanCapability::DynamicRenderingLocalRead));
    assert(support.is_enabled(VulkanCapability::ExtendedDynamicState1));
    assert(support.is_enabled(VulkanCapability::ExtendedDynamicState2));
    assert(support.is_enabled(VulkanCapability::ExtendedDynamicState2LogicOp));
    assert(!support.is_enabled(VulkanCapability::ExtendedDynamicState2PatchControlPoints));
    assert(support.is_enabled(VulkanCapability::ExtendedDynamicState3));
    assert(vulkan_eds3_feature_count(support.eds3_enabled) == 3);
    assert(support.is_enabled_eds3(VulkanEds3Feature::PolygonMode));
    assert(!support.is_enabled_eds3(VulkanEds3Feature::RasterizationSamples));
    assert(support.is_enabled(VulkanCapability::DescriptorBuffer));
    assert(!support.is_enabled(VulkanCapability::GraphicsPipelineLibrary));
    assert(support.is_enabled(VulkanCapability::ShaderModuleIdentifier));
    assert(support.is_enabled(VulkanCapability::UnifiedImageLayouts));
    assert(support.is_enabled(VulkanCapability::PresentId));
    assert(support.is_enabled(VulkanCapability::PresentWait));
    assert(!support.is_enabled(VulkanCapability::PresentId2));
    assert(!support.is_enabled(VulkanCapability::PresentWait2));
    assert(!support.is_enabled(VulkanCapability::PresentTiming));

    // Vulkan 1.3 makes EDS1 and the base EDS2 command set core, but the
    // EDS2 logic-op and patch-control-points features stay extension-gated.
    g_api_version = VK_API_VERSION_1_3;
    VulkanCapabilitySnapshot vk13_support{};
    assert(probe_vulkan_capability_support(physical, dispatch, &vk13_support));
    assert(vk13_support.supports(VulkanCapability::Vulkan13));
    assert(!vk13_support.supports(VulkanCapability::Vulkan14));
    assert(vk13_support.supports(VulkanCapability::ExtendedDynamicState1));
    assert(vk13_support.supports(VulkanCapability::ExtendedDynamicState2));
    assert(vk13_support.supports(VulkanCapability::ExtendedDynamicState2LogicOp));
    assert(!vk13_support.supports(VulkanCapability::ExtendedDynamicState2PatchControlPoints));
    assert(vk13_support.is_enabled(VulkanCapability::ExtendedDynamicState1));
    assert(vk13_support.is_enabled(VulkanCapability::ExtendedDynamicState2));
    assert(!vk13_support.knows_enabled(VulkanCapability::ExtendedDynamicState2LogicOp));

    // An exact empty device-create contract means optional device features are
    // known disabled; support-only translation probes must not make this claim.
    const std::span<const char* const> no_extensions{};
    observe_vulkan_enabled_state(no_extensions, nullptr, &vk13_support);
    assert(vk13_support.knows_enabled(VulkanCapability::ExtendedDynamicState3));
    assert(!vk13_support.is_enabled(VulkanCapability::ExtendedDynamicState3));
    assert(vk13_support.eds3_enabled == 0);
    assert(vk13_support.knows_enabled_eds3(VulkanEds3Feature::PolygonMode));
    assert(!vk13_support.is_enabled_eds3(VulkanEds3Feature::PolygonMode));

    // Enabling the EDS3 extension family without any individual feature bits
    // keeps the root enabled while correctly reporting zero usable EDS3 states.
    const char* eds3_only[] = {VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME};
    observe_vulkan_enabled_state(eds3_only, nullptr, &vk13_support);
    assert(vk13_support.is_enabled(VulkanCapability::ExtendedDynamicState3));
    assert(vk13_support.eds3_enabled == 0);

    return 0;
}
