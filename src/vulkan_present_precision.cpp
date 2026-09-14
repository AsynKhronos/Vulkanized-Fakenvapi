#include "vulkan_present_precision.h"

PresentPrecisionTier select_present_precision_tier(
    const PresentPrecisionCapabilities& caps,
    const PresentPrecisionObservation& observation) noexcept {
    if (!observation.has_present_id)
        return PresentPrecisionTier::CoreWsi;

#ifdef VK_EXT_present_timing
    if (caps.present_timing && caps.present_id2 && observation.uses_present_id2 &&
        observation.timing_requested &&
        (observation.swapchain_flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) != 0) {
        return PresentPrecisionTier::PresentTiming;
    }
#endif

#ifdef VK_KHR_present_wait2
    if (caps.present_wait2 && caps.present_id2 && observation.uses_present_id2 &&
        (observation.swapchain_flags & VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR) != 0) {
        return PresentPrecisionTier::PresentWait2;
    }
#endif

    if (caps.present_wait && caps.present_id)
        return PresentPrecisionTier::PresentWait;

    if ((observation.uses_present_id2 && caps.present_id2) || caps.present_id)
        return PresentPrecisionTier::PresentId;

    return PresentPrecisionTier::CoreWsi;
}

const char* present_precision_tier_name(PresentPrecisionTier tier) noexcept {
    switch (tier) {
        case PresentPrecisionTier::CoreWsi: return "VF0-core-wsi";
        case PresentPrecisionTier::PresentId: return "VF1-present-id";
        case PresentPrecisionTier::PresentWait: return "VF2-present-wait";
        case PresentPrecisionTier::PresentWait2: return "VF3-present-wait2";
        case PresentPrecisionTier::PresentTiming: return "VF4-present-timing";
    }
    return "VF0-core-wsi";
}
