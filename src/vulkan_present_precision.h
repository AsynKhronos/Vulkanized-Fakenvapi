#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>

enum class PresentPrecisionTier : std::uint8_t {
    CoreWsi = 0,
    PresentId = 1,
    PresentWait = 2,
    PresentWait2 = 3,
    PresentTiming = 4,
};

struct PresentPrecisionCapabilities {
    bool present_id = false;
    bool present_id2 = false;
    bool present_wait = false;
    bool present_wait2 = false;
    bool present_timing = false;
};

struct PresentPrecisionObservation {
    bool has_present_id = false;
    bool uses_present_id2 = false;
    bool timing_requested = false;
    VkSwapchainCreateFlagsKHR swapchain_flags = 0;
};

[[nodiscard]] PresentPrecisionTier select_present_precision_tier(
    const PresentPrecisionCapabilities& caps,
    const PresentPrecisionObservation& observation) noexcept;

[[nodiscard]] const char* present_precision_tier_name(PresentPrecisionTier tier) noexcept;
