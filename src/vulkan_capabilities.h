#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <span>

// R3.9 transport-neutral Vulkan capability model. Discovery is control-plane
// only: it never mutates VkDeviceCreateInfo, extension lists, feature chains,
// pipelines, command buffers, queues or swapchains.
enum class VulkanCapability : std::uint8_t {
    Vulkan13 = 0,
    Vulkan14,
    TimelineSemaphore,
    Synchronization2,
    DynamicRendering,
    DynamicRenderingLocalRead,
    ExtendedDynamicState1,
    ExtendedDynamicState2,
    ExtendedDynamicState2LogicOp,
    ExtendedDynamicState2PatchControlPoints,
    ExtendedDynamicState3,
    DescriptorBuffer,
    GraphicsPipelineLibrary,
    ShaderModuleIdentifier,
    UnifiedImageLayouts,
    CalibratedTimestamps,
    Maintenance5,
    Maintenance6,
    PipelineRobustness,
    PresentId,
    PresentId2,
    PresentWait,
    PresentWait2,
    PresentTiming,
    Count,
};

static_assert(static_cast<unsigned>(VulkanCapability::Count) <= 64);

constexpr std::uint64_t vulkan_capability_bit(VulkanCapability capability) noexcept {
    return 1ull << static_cast<unsigned>(capability);
}

enum class VulkanEds3Feature : std::uint8_t {
    TessellationDomainOrigin = 0,
    DepthClampEnable,
    PolygonMode,
    RasterizationSamples,
    SampleMask,
    AlphaToCoverageEnable,
    AlphaToOneEnable,
    LogicOpEnable,
    ColorBlendEnable,
    ColorBlendEquation,
    ColorWriteMask,
    RasterizationStream,
    ConservativeRasterizationMode,
    ExtraPrimitiveOverestimationSize,
    DepthClipEnable,
    SampleLocationsEnable,
    ColorBlendAdvanced,
    ProvokingVertexMode,
    LineRasterizationMode,
    LineStippleEnable,
    DepthClipNegativeOneToOne,
    ViewportWScalingEnable,
    ViewportSwizzle,
    CoverageToColorEnable,
    CoverageToColorLocation,
    CoverageModulationMode,
    CoverageModulationTableEnable,
    CoverageModulationTable,
    CoverageReductionMode,
    RepresentativeFragmentTestEnable,
    ShadingRateImageEnable,
    Count,
};

static_assert(static_cast<unsigned>(VulkanEds3Feature::Count) <= 64);

constexpr std::uint64_t vulkan_eds3_bit(VulkanEds3Feature feature) noexcept {
    return 1ull << static_cast<unsigned>(feature);
}

struct VulkanCapabilitySnapshot {
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;

    // supported: physical-device support.
    // enabled: capability is known enabled on the concrete VkDevice.
    // enabled_known: whether enabled/disabled is known rather than inferred.
    std::uint64_t supported = 0;
    std::uint64_t enabled = 0;
    std::uint64_t enabled_known = 0;

    // EDS3 is deliberately granular. The root ExtendedDynamicState3 bit only
    // means the extension/capability family exists; individual bits live here.
    std::uint64_t eds3_supported = 0;
    std::uint64_t eds3_enabled = 0;
    std::uint64_t eds3_enabled_known = 0;

    bool support_probe_complete = false;

    [[nodiscard]] constexpr bool supports(VulkanCapability capability) const noexcept {
        return (supported & vulkan_capability_bit(capability)) != 0;
    }
    [[nodiscard]] constexpr bool knows_enabled(VulkanCapability capability) const noexcept {
        return (enabled_known & vulkan_capability_bit(capability)) != 0;
    }
    [[nodiscard]] constexpr bool is_enabled(VulkanCapability capability) const noexcept {
        return knows_enabled(capability) &&
               (enabled & vulkan_capability_bit(capability)) != 0;
    }
    [[nodiscard]] constexpr bool supports_eds3(VulkanEds3Feature feature) const noexcept {
        return (eds3_supported & vulkan_eds3_bit(feature)) != 0;
    }
    [[nodiscard]] constexpr bool knows_enabled_eds3(VulkanEds3Feature feature) const noexcept {
        return (eds3_enabled_known & vulkan_eds3_bit(feature)) != 0;
    }
    [[nodiscard]] constexpr bool is_enabled_eds3(VulkanEds3Feature feature) const noexcept {
        return knows_enabled_eds3(feature) &&
               (eds3_enabled & vulkan_eds3_bit(feature)) != 0;
    }
};

struct VulkanCapabilityProbeDispatch {
    PFN_vkGetPhysicalDeviceProperties get_properties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 get_features2 = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions = nullptr;
};

// Query physical-device support using only public Vulkan capability queries.
// No heap allocation and no device mutation are performed.
[[nodiscard]] bool probe_vulkan_capability_support(
    VkPhysicalDevice physical_device,
    const VulkanCapabilityProbeDispatch& dispatch,
    VulkanCapabilitySnapshot* snapshot) noexcept;

// Merge the exact application-enabled state from VkDeviceCreateInfo. This is
// safe to call before vkCreateDevice; it never modifies the create info.
void observe_vulkan_device_create_info(
    const VkDeviceCreateInfo& create_info,
    VulkanCapabilitySnapshot* snapshot) noexcept;

// Merge enabled extension names / feature pNext supplied by a translation
// layer's public interop contract. Passing a null feature chain is valid.
void observe_vulkan_enabled_state(
    std::span<const char* const> enabled_extensions,
    const void* enabled_feature_chain,
    VulkanCapabilitySnapshot* snapshot) noexcept;

[[nodiscard]] constexpr unsigned vulkan_eds3_feature_count(std::uint64_t mask) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_popcountll(mask));
#else
    unsigned count = 0;
    while (mask) {
        count += static_cast<unsigned>(mask & 1ull);
        mask >>= 1u;
    }
    return count;
#endif
}

[[nodiscard]] const char* vulkan_capability_name(VulkanCapability capability) noexcept;
[[nodiscard]] const char* vulkan_eds3_feature_name(VulkanEds3Feature feature) noexcept;
