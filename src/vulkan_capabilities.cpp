#include "vulkan_capabilities.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {
constexpr std::uint32_t kMaxDeviceExtensions = 512;

void set_supported(VulkanCapabilitySnapshot& snapshot, VulkanCapability capability, bool value = true) noexcept {
    const auto bit = vulkan_capability_bit(capability);
    if (value) snapshot.supported |= bit;
    else snapshot.supported &= ~bit;
}

void set_enabled_known(
    VulkanCapabilitySnapshot& snapshot,
    VulkanCapability capability,
    bool enabled) noexcept {
    const auto bit = vulkan_capability_bit(capability);
    snapshot.enabled_known |= bit;
    if (enabled) snapshot.enabled |= bit;
    else snapshot.enabled &= ~bit;
}

bool has_extension(
    const VkExtensionProperties* extensions,
    std::uint32_t count,
    const char* name) noexcept {
    if (!name) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(extensions[i].extensionName, name) == 0)
            return true;
    }
    return false;
}

bool has_enabled_extension(std::span<const char* const> extensions, const char* name) noexcept {
    if (!name) return false;
    for (const char* extension : extensions) {
        if (extension && std::strcmp(extension, name) == 0)
            return true;
    }
    return false;
}

const VkBaseInStructure* find_structure(const void* chain, VkStructureType type) noexcept {
    auto* node = static_cast<const VkBaseInStructure*>(chain);
    while (node) {
        if (node->sType == type)
            return node;
        node = node->pNext;
    }
    return nullptr;
}

template <typename T>
void append_feature(VkBaseOutStructure*& tail, T& feature, VkStructureType type) noexcept {
    feature = {};
    feature.sType = type;
    feature.pNext = nullptr;
    tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&feature);
    tail = reinterpret_cast<VkBaseOutStructure*>(&feature);
}

std::uint64_t eds3_mask_from(const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT& f) noexcept {
    std::uint64_t mask = 0;
#define VKF_EDS3(field, token) \
    if (f.field == VK_TRUE) mask |= vulkan_eds3_bit(VulkanEds3Feature::token)
    VKF_EDS3(extendedDynamicState3TessellationDomainOrigin, TessellationDomainOrigin);
    VKF_EDS3(extendedDynamicState3DepthClampEnable, DepthClampEnable);
    VKF_EDS3(extendedDynamicState3PolygonMode, PolygonMode);
    VKF_EDS3(extendedDynamicState3RasterizationSamples, RasterizationSamples);
    VKF_EDS3(extendedDynamicState3SampleMask, SampleMask);
    VKF_EDS3(extendedDynamicState3AlphaToCoverageEnable, AlphaToCoverageEnable);
    VKF_EDS3(extendedDynamicState3AlphaToOneEnable, AlphaToOneEnable);
    VKF_EDS3(extendedDynamicState3LogicOpEnable, LogicOpEnable);
    VKF_EDS3(extendedDynamicState3ColorBlendEnable, ColorBlendEnable);
    VKF_EDS3(extendedDynamicState3ColorBlendEquation, ColorBlendEquation);
    VKF_EDS3(extendedDynamicState3ColorWriteMask, ColorWriteMask);
    VKF_EDS3(extendedDynamicState3RasterizationStream, RasterizationStream);
    VKF_EDS3(extendedDynamicState3ConservativeRasterizationMode, ConservativeRasterizationMode);
    VKF_EDS3(extendedDynamicState3ExtraPrimitiveOverestimationSize, ExtraPrimitiveOverestimationSize);
    VKF_EDS3(extendedDynamicState3DepthClipEnable, DepthClipEnable);
    VKF_EDS3(extendedDynamicState3SampleLocationsEnable, SampleLocationsEnable);
    VKF_EDS3(extendedDynamicState3ColorBlendAdvanced, ColorBlendAdvanced);
    VKF_EDS3(extendedDynamicState3ProvokingVertexMode, ProvokingVertexMode);
    VKF_EDS3(extendedDynamicState3LineRasterizationMode, LineRasterizationMode);
    VKF_EDS3(extendedDynamicState3LineStippleEnable, LineStippleEnable);
    VKF_EDS3(extendedDynamicState3DepthClipNegativeOneToOne, DepthClipNegativeOneToOne);
    VKF_EDS3(extendedDynamicState3ViewportWScalingEnable, ViewportWScalingEnable);
    VKF_EDS3(extendedDynamicState3ViewportSwizzle, ViewportSwizzle);
    VKF_EDS3(extendedDynamicState3CoverageToColorEnable, CoverageToColorEnable);
    VKF_EDS3(extendedDynamicState3CoverageToColorLocation, CoverageToColorLocation);
    VKF_EDS3(extendedDynamicState3CoverageModulationMode, CoverageModulationMode);
    VKF_EDS3(extendedDynamicState3CoverageModulationTableEnable, CoverageModulationTableEnable);
    VKF_EDS3(extendedDynamicState3CoverageModulationTable, CoverageModulationTable);
    VKF_EDS3(extendedDynamicState3CoverageReductionMode, CoverageReductionMode);
    VKF_EDS3(extendedDynamicState3RepresentativeFragmentTestEnable, RepresentativeFragmentTestEnable);
    VKF_EDS3(extendedDynamicState3ShadingRateImageEnable, ShadingRateImageEnable);
#undef VKF_EDS3
    return mask;
}

void mark_extension_feature_enabled(
    VulkanCapabilitySnapshot& snapshot,
    VulkanCapability capability,
    bool extension_enabled,
    bool feature_enabled) noexcept {
    set_enabled_known(snapshot, capability, extension_enabled && feature_enabled);
}

} // namespace

bool probe_vulkan_capability_support(
    VkPhysicalDevice physical_device,
    const VulkanCapabilityProbeDispatch& dispatch,
    VulkanCapabilitySnapshot* snapshot) noexcept {
    if (physical_device == VK_NULL_HANDLE || !snapshot || !dispatch.get_properties)
        return false;

    VulkanCapabilitySnapshot result{};

    VkPhysicalDeviceProperties properties{};
    dispatch.get_properties(physical_device, &properties);
    result.api_version = properties.apiVersion;
    result.driver_version = properties.driverVersion;
    result.vendor_id = properties.vendorID;
    result.device_id = properties.deviceID;

    const bool vk12 = result.api_version >= VK_API_VERSION_1_2;
    const bool vk13 = result.api_version >= VK_API_VERSION_1_3;
    const bool vk14 = result.api_version >= VK_API_VERSION_1_4;
    set_supported(result, VulkanCapability::Vulkan13, vk13);
    set_supported(result, VulkanCapability::Vulkan14, vk14);
    // Core-version presence is not an opt-in VkDevice feature.
    set_enabled_known(result, VulkanCapability::Vulkan13, vk13);
    set_enabled_known(result, VulkanCapability::Vulkan14, vk14);

    std::array<VkExtensionProperties, kMaxDeviceExtensions> extensions{};
    std::uint32_t reported_count = 0;
    std::uint32_t extension_count = 0;
    if (dispatch.enumerate_extensions) {
        VkResult query = dispatch.enumerate_extensions(
            physical_device, nullptr, &reported_count, nullptr);
        if (query == VK_SUCCESS) {
            extension_count = std::min(reported_count, kMaxDeviceExtensions);
            std::uint32_t capacity = extension_count;
            if (capacity == 0) {
                result.support_probe_complete = true;
            } else {
                query = dispatch.enumerate_extensions(
                    physical_device, nullptr, &capacity, extensions.data());
                extension_count = std::min(capacity, kMaxDeviceExtensions);
                result.support_probe_complete =
                    query == VK_SUCCESS && reported_count <= kMaxDeviceExtensions;
            }
        }
    }

    const auto ext = [&](const char* name) noexcept {
        return has_extension(extensions.data(), extension_count, name);
    };

    const bool ext_timeline = ext(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    const bool ext_sync2 = ext(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    const bool ext_dynamic_rendering = ext(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    const bool ext_eds1 = ext(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    const bool ext_eds2 = ext(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    const bool ext_eds3 = ext(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    const bool ext_descriptor_buffer = ext(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    const bool ext_gpl = ext(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    const bool ext_shader_id = ext(VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME);
    const bool ext_unified_layouts = ext(VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
    const bool ext_dynamic_local_read = ext(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    const bool ext_maintenance5 = ext(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    const bool ext_maintenance6 = ext(VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
    const bool ext_pipeline_robustness = ext(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
    const bool ext_present_id = ext(VK_KHR_PRESENT_ID_EXTENSION_NAME);
#ifdef VK_KHR_present_id2
    const bool ext_present_id2 = ext(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
#else
    const bool ext_present_id2 = false;
#endif
    const bool ext_present_wait = ext(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
#ifdef VK_KHR_present_wait2
    const bool ext_present_wait2 = ext(VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
#else
    const bool ext_present_wait2 = false;
#endif
#ifdef VK_EXT_present_timing
    const bool ext_present_timing = ext(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
#else
    const bool ext_present_timing = false;
#endif
#ifdef VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
    const bool ext_calibrated = ext(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ||
        ext(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#else
    const bool ext_calibrated = ext(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#endif

    set_supported(result, VulkanCapability::CalibratedTimestamps, ext_calibrated);

    if (!dispatch.get_features2) {
        // Featureless/core-required capabilities can still be represented.
        set_supported(result, VulkanCapability::ExtendedDynamicState1, vk13);
        set_supported(result, VulkanCapability::ExtendedDynamicState2, vk13);
        set_enabled_known(result, VulkanCapability::ExtendedDynamicState1, vk13);
        set_enabled_known(result, VulkanCapability::ExtendedDynamicState2, vk13);
        *snapshot = result;
        return true;
    }

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    auto* tail = reinterpret_cast<VkBaseOutStructure*>(&features2);

    VkPhysicalDeviceVulkan12Features f12{};
    VkPhysicalDeviceVulkan13Features f13{};
    VkPhysicalDeviceVulkan14Features f14{};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
    VkPhysicalDeviceSynchronization2Features sync2{};
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering{};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds1{};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2{};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer{};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{};
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT shader_id{};
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unified_layouts{};
    VkPhysicalDeviceDynamicRenderingLocalReadFeatures local_read{};
    VkPhysicalDeviceMaintenance5Features maintenance5{};
    VkPhysicalDeviceMaintenance6Features maintenance6{};
    VkPhysicalDevicePipelineRobustnessFeatures pipeline_robustness{};
    VkPhysicalDevicePresentIdFeaturesKHR present_id{};
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait{};
#ifdef VK_KHR_present_id2
    VkPhysicalDevicePresentId2FeaturesKHR present_id2{};
#endif
#ifdef VK_KHR_present_wait2
    VkPhysicalDevicePresentWait2FeaturesKHR present_wait2{};
#endif
#ifdef VK_EXT_present_timing
    VkPhysicalDevicePresentTimingFeaturesEXT present_timing{};
#endif

    if (vk12) append_feature(tail, f12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    else if (ext_timeline) append_feature(tail, timeline, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);

    if (vk13) append_feature(tail, f13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    else {
        if (ext_sync2) append_feature(tail, sync2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES);
        if (ext_dynamic_rendering) append_feature(tail, dynamic_rendering, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
    }

    if (vk14) append_feature(tail, f14, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES);
    else {
        if (ext_dynamic_local_read) append_feature(tail, local_read, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES);
        if (ext_maintenance5) append_feature(tail, maintenance5, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES);
        if (ext_maintenance6) append_feature(tail, maintenance6, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES);
        if (ext_pipeline_robustness) append_feature(tail, pipeline_robustness, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES);
    }

    if (ext_eds1) append_feature(tail, eds1, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT);
    if (ext_eds2) append_feature(tail, eds2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT);
    if (ext_eds3) append_feature(tail, eds3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
    if (ext_descriptor_buffer) append_feature(tail, descriptor_buffer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT);
    if (ext_gpl) append_feature(tail, gpl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT);
    if (ext_shader_id) append_feature(tail, shader_id, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT);
    if (ext_unified_layouts) append_feature(tail, unified_layouts, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR);
    if (ext_present_id) append_feature(tail, present_id, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
    if (ext_present_wait) append_feature(tail, present_wait, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
#ifdef VK_KHR_present_id2
    if (ext_present_id2) append_feature(tail, present_id2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR);
#endif
#ifdef VK_KHR_present_wait2
    if (ext_present_wait2) append_feature(tail, present_wait2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR);
#endif
#ifdef VK_EXT_present_timing
    if (ext_present_timing) append_feature(tail, present_timing, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT);
#endif

    dispatch.get_features2(physical_device, &features2);

    const bool timeline_supported = vk12 ? f12.timelineSemaphore == VK_TRUE : timeline.timelineSemaphore == VK_TRUE;
    const bool sync2_supported = vk13 ? f13.synchronization2 == VK_TRUE : sync2.synchronization2 == VK_TRUE;
    const bool dynamic_supported = vk13 ? f13.dynamicRendering == VK_TRUE : dynamic_rendering.dynamicRendering == VK_TRUE;
    const bool local_read_supported = vk14 ? f14.dynamicRenderingLocalRead == VK_TRUE : local_read.dynamicRenderingLocalRead == VK_TRUE;
    const bool maintenance5_supported = vk14 ? f14.maintenance5 == VK_TRUE : maintenance5.maintenance5 == VK_TRUE;
    const bool maintenance6_supported = vk14 ? f14.maintenance6 == VK_TRUE : maintenance6.maintenance6 == VK_TRUE;
    const bool pipeline_robustness_supported = vk14 ? f14.pipelineRobustness == VK_TRUE : pipeline_robustness.pipelineRobustness == VK_TRUE;

    set_supported(result, VulkanCapability::TimelineSemaphore, timeline_supported);
    set_supported(result, VulkanCapability::Synchronization2, sync2_supported);
    set_supported(result, VulkanCapability::DynamicRendering, dynamic_supported);
    set_supported(result, VulkanCapability::DynamicRenderingLocalRead, local_read_supported);
    set_supported(result, VulkanCapability::Maintenance5, maintenance5_supported);
    set_supported(result, VulkanCapability::Maintenance6, maintenance6_supported);
    set_supported(result, VulkanCapability::PipelineRobustness, pipeline_robustness_supported);

    const bool eds1_supported = vk13 || (ext_eds1 && eds1.extendedDynamicState == VK_TRUE);
    const bool eds2_supported = vk13 || (ext_eds2 && eds2.extendedDynamicState2 == VK_TRUE);
    set_supported(result, VulkanCapability::ExtendedDynamicState1, eds1_supported);
    set_supported(result, VulkanCapability::ExtendedDynamicState2, eds2_supported);
    set_supported(result, VulkanCapability::ExtendedDynamicState2LogicOp,
                  ext_eds2 && eds2.extendedDynamicState2LogicOp == VK_TRUE);
    set_supported(result, VulkanCapability::ExtendedDynamicState2PatchControlPoints,
                  ext_eds2 && eds2.extendedDynamicState2PatchControlPoints == VK_TRUE);

    if (ext_eds3) {
        set_supported(result, VulkanCapability::ExtendedDynamicState3, true);
        result.eds3_supported = eds3_mask_from(eds3);
    }

    set_supported(result, VulkanCapability::DescriptorBuffer,
                  ext_descriptor_buffer && descriptor_buffer.descriptorBuffer == VK_TRUE);
    set_supported(result, VulkanCapability::GraphicsPipelineLibrary,
                  ext_gpl && gpl.graphicsPipelineLibrary == VK_TRUE);
    set_supported(result, VulkanCapability::ShaderModuleIdentifier,
                  ext_shader_id && shader_id.shaderModuleIdentifier == VK_TRUE);
    set_supported(result, VulkanCapability::UnifiedImageLayouts,
                  ext_unified_layouts && unified_layouts.unifiedImageLayouts == VK_TRUE);
    set_supported(result, VulkanCapability::PresentId,
                  ext_present_id && present_id.presentId == VK_TRUE);
    set_supported(result, VulkanCapability::PresentWait,
                  ext_present_wait && present_wait.presentWait == VK_TRUE);
#ifdef VK_KHR_present_id2
    set_supported(result, VulkanCapability::PresentId2,
                  ext_present_id2 && present_id2.presentId2 == VK_TRUE);
#endif
#ifdef VK_KHR_present_wait2
    set_supported(result, VulkanCapability::PresentWait2,
                  ext_present_wait2 && present_wait2.presentWait2 == VK_TRUE);
#endif
#ifdef VK_EXT_present_timing
    set_supported(result, VulkanCapability::PresentTiming,
                  ext_present_timing && present_timing.presentTiming == VK_TRUE);
#endif

    // EDS1 and the core subset of EDS2 do not have Vulkan 1.3 feature bits;
    // their commands are core functionality once a 1.3 device exists.
    if (vk13) {
        set_enabled_known(result, VulkanCapability::ExtendedDynamicState1, true);
        set_enabled_known(result, VulkanCapability::ExtendedDynamicState2, true);
    }

    *snapshot = result;
    return true;
}

void observe_vulkan_device_create_info(
    const VkDeviceCreateInfo& create_info,
    VulkanCapabilitySnapshot* snapshot) noexcept {
    if (!snapshot) return;
    const std::span<const char* const> extensions{
        create_info.ppEnabledExtensionNames,
        create_info.ppEnabledExtensionNames ? create_info.enabledExtensionCount : 0u};
    observe_vulkan_enabled_state(extensions, create_info.pNext, snapshot);
}

void observe_vulkan_enabled_state(
    std::span<const char* const> extensions,
    const void* chain,
    VulkanCapabilitySnapshot* snapshot) noexcept {
    if (!snapshot) return;

    const bool vk12 = snapshot->api_version >= VK_API_VERSION_1_2;
    const bool vk13 = snapshot->api_version >= VK_API_VERSION_1_3;
    const bool vk14 = snapshot->api_version >= VK_API_VERSION_1_4;

    // Core version and required EDS1/EDS2-core state are known immediately.
    set_enabled_known(*snapshot, VulkanCapability::Vulkan13, vk13);
    set_enabled_known(*snapshot, VulkanCapability::Vulkan14, vk14);
    set_enabled_known(*snapshot, VulkanCapability::ExtendedDynamicState1,
                      vk13 || (has_enabled_extension(extensions, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) &&
                               find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT) &&
                               reinterpret_cast<const VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*>(
                                   find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT))->extendedDynamicState == VK_TRUE));
    set_enabled_known(*snapshot, VulkanCapability::ExtendedDynamicState2,
                      vk13 || (has_enabled_extension(extensions, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) &&
                               find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT) &&
                               reinterpret_cast<const VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*>(
                                   find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT))->extendedDynamicState2 == VK_TRUE));

    const auto* f12 = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES));
    const auto* f13 = reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES));
    const auto* f14 = reinterpret_cast<const VkPhysicalDeviceVulkan14Features*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES));
    const auto* timeline = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES));
    const auto* sync2 = reinterpret_cast<const VkPhysicalDeviceSynchronization2Features*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES));
    const auto* dynamic = reinterpret_cast<const VkPhysicalDeviceDynamicRenderingFeatures*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES));
    const auto* local_read = reinterpret_cast<const VkPhysicalDeviceDynamicRenderingLocalReadFeatures*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES));
    const auto* maintenance5 = reinterpret_cast<const VkPhysicalDeviceMaintenance5Features*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES));
    const auto* maintenance6 = reinterpret_cast<const VkPhysicalDeviceMaintenance6Features*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES));
    const auto* robustness = reinterpret_cast<const VkPhysicalDevicePipelineRobustnessFeatures*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES));

    set_enabled_known(*snapshot, VulkanCapability::TimelineSemaphore,
        f12 ? f12->timelineSemaphore == VK_TRUE : (timeline ? timeline->timelineSemaphore == VK_TRUE : false));
    set_enabled_known(*snapshot, VulkanCapability::Synchronization2,
        f13 ? f13->synchronization2 == VK_TRUE : (sync2 ? sync2->synchronization2 == VK_TRUE : false));
    set_enabled_known(*snapshot, VulkanCapability::DynamicRendering,
        f13 ? f13->dynamicRendering == VK_TRUE : (dynamic ? dynamic->dynamicRendering == VK_TRUE : false));
    set_enabled_known(*snapshot, VulkanCapability::DynamicRenderingLocalRead,
        f14 ? f14->dynamicRenderingLocalRead == VK_TRUE : (local_read ? local_read->dynamicRenderingLocalRead == VK_TRUE : false));
    set_enabled_known(*snapshot, VulkanCapability::Maintenance5,
        f14 ? f14->maintenance5 == VK_TRUE : (maintenance5 ? maintenance5->maintenance5 == VK_TRUE : false));
    set_enabled_known(*snapshot, VulkanCapability::Maintenance6,
        f14 ? f14->maintenance6 == VK_TRUE : (maintenance6 ? maintenance6->maintenance6 == VK_TRUE : false));
    set_enabled_known(*snapshot, VulkanCapability::PipelineRobustness,
        f14 ? f14->pipelineRobustness == VK_TRUE : (robustness ? robustness->pipelineRobustness == VK_TRUE : false));

    const bool eds2_extension_enabled = has_enabled_extension(extensions, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    const auto* eds2 = reinterpret_cast<const VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT));
    set_enabled_known(*snapshot, VulkanCapability::ExtendedDynamicState2LogicOp,
        eds2_extension_enabled && eds2 && eds2->extendedDynamicState2LogicOp == VK_TRUE);
    set_enabled_known(*snapshot, VulkanCapability::ExtendedDynamicState2PatchControlPoints,
        eds2_extension_enabled && eds2 && eds2->extendedDynamicState2PatchControlPoints == VK_TRUE);

    const bool eds3_extension_enabled = has_enabled_extension(extensions, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    const auto* eds3 = reinterpret_cast<const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT));
    // Root EDS3 state tracks extension enablement. Actual usable dynamic states
    // remain represented exclusively by the individual EDS3 feature mask.
    set_enabled_known(*snapshot, VulkanCapability::ExtendedDynamicState3, eds3_extension_enabled);
    snapshot->eds3_enabled_known = ~0ull >> (64u - static_cast<unsigned>(VulkanEds3Feature::Count));
    snapshot->eds3_enabled = (eds3_extension_enabled && eds3) ? eds3_mask_from(*eds3) : 0;

    // Avoid pointer-to-member machinery for MinGW portability; keep the
    // remaining feature probes explicit and mechanically auditable.
    const auto* descriptor = reinterpret_cast<const VkPhysicalDeviceDescriptorBufferFeaturesEXT*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT));
    set_enabled_known(*snapshot, VulkanCapability::DescriptorBuffer,
        has_enabled_extension(extensions, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) && descriptor && descriptor->descriptorBuffer == VK_TRUE);

    const auto* gpl = reinterpret_cast<const VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT));
    set_enabled_known(*snapshot, VulkanCapability::GraphicsPipelineLibrary,
        has_enabled_extension(extensions, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && gpl && gpl->graphicsPipelineLibrary == VK_TRUE);

    const auto* shader_id = reinterpret_cast<const VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT));
    set_enabled_known(*snapshot, VulkanCapability::ShaderModuleIdentifier,
        has_enabled_extension(extensions, VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME) && shader_id && shader_id->shaderModuleIdentifier == VK_TRUE);

    const auto* unified = reinterpret_cast<const VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR));
    set_enabled_known(*snapshot, VulkanCapability::UnifiedImageLayouts,
        has_enabled_extension(extensions, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME) && unified && unified->unifiedImageLayouts == VK_TRUE);

#ifdef VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
    set_enabled_known(*snapshot, VulkanCapability::CalibratedTimestamps,
        has_enabled_extension(extensions, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ||
        has_enabled_extension(extensions, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME));
#else
    set_enabled_known(*snapshot, VulkanCapability::CalibratedTimestamps,
        has_enabled_extension(extensions, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME));
#endif

    const auto* present_id = reinterpret_cast<const VkPhysicalDevicePresentIdFeaturesKHR*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR));
    mark_extension_feature_enabled(*snapshot, VulkanCapability::PresentId,
        has_enabled_extension(extensions, VK_KHR_PRESENT_ID_EXTENSION_NAME),
        present_id && present_id->presentId == VK_TRUE);

    const auto* present_wait = reinterpret_cast<const VkPhysicalDevicePresentWaitFeaturesKHR*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR));
    mark_extension_feature_enabled(*snapshot, VulkanCapability::PresentWait,
        has_enabled_extension(extensions, VK_KHR_PRESENT_WAIT_EXTENSION_NAME),
        present_wait && present_wait->presentWait == VK_TRUE);
#ifdef VK_KHR_present_id2
    const auto* present_id2 = reinterpret_cast<const VkPhysicalDevicePresentId2FeaturesKHR*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR));
    mark_extension_feature_enabled(*snapshot, VulkanCapability::PresentId2,
        has_enabled_extension(extensions, VK_KHR_PRESENT_ID_2_EXTENSION_NAME),
        present_id2 && present_id2->presentId2 == VK_TRUE);
#endif
#ifdef VK_KHR_present_wait2
    const auto* present_wait2 = reinterpret_cast<const VkPhysicalDevicePresentWait2FeaturesKHR*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR));
    mark_extension_feature_enabled(*snapshot, VulkanCapability::PresentWait2,
        has_enabled_extension(extensions, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME),
        present_wait2 && present_wait2->presentWait2 == VK_TRUE);
#endif
#ifdef VK_EXT_present_timing
    const auto* present_timing = reinterpret_cast<const VkPhysicalDevicePresentTimingFeaturesEXT*>(
        find_structure(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT));
    mark_extension_feature_enabled(*snapshot, VulkanCapability::PresentTiming,
        has_enabled_extension(extensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME),
        present_timing && present_timing->presentTiming == VK_TRUE);
#endif

    (void)vk12;
}

const char* vulkan_capability_name(VulkanCapability capability) noexcept {
    switch (capability) {
        case VulkanCapability::Vulkan13: return "vulkan13";
        case VulkanCapability::Vulkan14: return "vulkan14";
        case VulkanCapability::TimelineSemaphore: return "timeline_semaphore";
        case VulkanCapability::Synchronization2: return "synchronization2";
        case VulkanCapability::DynamicRendering: return "dynamic_rendering";
        case VulkanCapability::DynamicRenderingLocalRead: return "dynamic_rendering_local_read";
        case VulkanCapability::ExtendedDynamicState1: return "eds1";
        case VulkanCapability::ExtendedDynamicState2: return "eds2";
        case VulkanCapability::ExtendedDynamicState2LogicOp: return "eds2_logic_op";
        case VulkanCapability::ExtendedDynamicState2PatchControlPoints: return "eds2_patch_control_points";
        case VulkanCapability::ExtendedDynamicState3: return "eds3";
        case VulkanCapability::DescriptorBuffer: return "descriptor_buffer";
        case VulkanCapability::GraphicsPipelineLibrary: return "graphics_pipeline_library";
        case VulkanCapability::ShaderModuleIdentifier: return "shader_module_identifier";
        case VulkanCapability::UnifiedImageLayouts: return "unified_image_layouts";
        case VulkanCapability::CalibratedTimestamps: return "calibrated_timestamps";
        case VulkanCapability::Maintenance5: return "maintenance5";
        case VulkanCapability::Maintenance6: return "maintenance6";
        case VulkanCapability::PipelineRobustness: return "pipeline_robustness";
        case VulkanCapability::PresentId: return "present_id";
        case VulkanCapability::PresentId2: return "present_id2";
        case VulkanCapability::PresentWait: return "present_wait";
        case VulkanCapability::PresentWait2: return "present_wait2";
        case VulkanCapability::PresentTiming: return "present_timing";
        case VulkanCapability::Count: break;
    }
    return "unknown";
}

const char* vulkan_eds3_feature_name(VulkanEds3Feature feature) noexcept {
    switch (feature) {
        case VulkanEds3Feature::TessellationDomainOrigin: return "tessellationDomainOrigin";
        case VulkanEds3Feature::DepthClampEnable: return "depthClampEnable";
        case VulkanEds3Feature::PolygonMode: return "polygonMode";
        case VulkanEds3Feature::RasterizationSamples: return "rasterizationSamples";
        case VulkanEds3Feature::SampleMask: return "sampleMask";
        case VulkanEds3Feature::AlphaToCoverageEnable: return "alphaToCoverageEnable";
        case VulkanEds3Feature::AlphaToOneEnable: return "alphaToOneEnable";
        case VulkanEds3Feature::LogicOpEnable: return "logicOpEnable";
        case VulkanEds3Feature::ColorBlendEnable: return "colorBlendEnable";
        case VulkanEds3Feature::ColorBlendEquation: return "colorBlendEquation";
        case VulkanEds3Feature::ColorWriteMask: return "colorWriteMask";
        case VulkanEds3Feature::RasterizationStream: return "rasterizationStream";
        case VulkanEds3Feature::ConservativeRasterizationMode: return "conservativeRasterizationMode";
        case VulkanEds3Feature::ExtraPrimitiveOverestimationSize: return "extraPrimitiveOverestimationSize";
        case VulkanEds3Feature::DepthClipEnable: return "depthClipEnable";
        case VulkanEds3Feature::SampleLocationsEnable: return "sampleLocationsEnable";
        case VulkanEds3Feature::ColorBlendAdvanced: return "colorBlendAdvanced";
        case VulkanEds3Feature::ProvokingVertexMode: return "provokingVertexMode";
        case VulkanEds3Feature::LineRasterizationMode: return "lineRasterizationMode";
        case VulkanEds3Feature::LineStippleEnable: return "lineStippleEnable";
        case VulkanEds3Feature::DepthClipNegativeOneToOne: return "depthClipNegativeOneToOne";
        case VulkanEds3Feature::ViewportWScalingEnable: return "viewportWScalingEnable";
        case VulkanEds3Feature::ViewportSwizzle: return "viewportSwizzle";
        case VulkanEds3Feature::CoverageToColorEnable: return "coverageToColorEnable";
        case VulkanEds3Feature::CoverageToColorLocation: return "coverageToColorLocation";
        case VulkanEds3Feature::CoverageModulationMode: return "coverageModulationMode";
        case VulkanEds3Feature::CoverageModulationTableEnable: return "coverageModulationTableEnable";
        case VulkanEds3Feature::CoverageModulationTable: return "coverageModulationTable";
        case VulkanEds3Feature::CoverageReductionMode: return "coverageReductionMode";
        case VulkanEds3Feature::RepresentativeFragmentTestEnable: return "representativeFragmentTestEnable";
        case VulkanEds3Feature::ShadingRateImageEnable: return "shadingRateImageEnable";
        case VulkanEds3Feature::Count: break;
    }
    return "unknown";
}
