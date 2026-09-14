#include "vulkan_present_precision.h"

#include <cassert>
#include <cstring>

int main() {
    PresentPrecisionCapabilities caps{};
    PresentPrecisionObservation obs{};

    assert(select_present_precision_tier(caps, obs) == PresentPrecisionTier::CoreWsi);

    obs.has_present_id = true;
    caps.present_id = true;
    assert(select_present_precision_tier(caps, obs) == PresentPrecisionTier::PresentId);

    caps.present_wait = true;
    assert(select_present_precision_tier(caps, obs) == PresentPrecisionTier::PresentWait);

#ifdef VK_KHR_present_wait2
    caps.present_id2 = true;
    caps.present_wait2 = true;
    obs.uses_present_id2 = true;
    obs.swapchain_flags = VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    assert(select_present_precision_tier(caps, obs) == PresentPrecisionTier::PresentWait2);

    // present_wait2 is swapchain opt-in; extension/device support alone is not enough.
    obs.swapchain_flags = 0;
    assert(select_present_precision_tier(caps, obs) == PresentPrecisionTier::PresentWait);
#endif

#ifdef VK_EXT_present_timing
    caps.present_timing = true;
    obs.uses_present_id2 = true;
    obs.timing_requested = true;
    obs.swapchain_flags = VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
    assert(select_present_precision_tier(caps, obs) == PresentPrecisionTier::PresentTiming);

    // Timing also requires a per-present stage query and a timing-enabled swapchain.
    obs.timing_requested = false;
    assert(select_present_precision_tier(caps, obs) == PresentPrecisionTier::PresentWait);
#endif

    assert(std::strcmp(present_precision_tier_name(PresentPrecisionTier::CoreWsi), "VF0-core-wsi") == 0);
    assert(std::strcmp(present_precision_tier_name(PresentPrecisionTier::PresentId), "VF1-present-id") == 0);
    assert(std::strcmp(present_precision_tier_name(PresentPrecisionTier::PresentWait), "VF2-present-wait") == 0);
    assert(std::strcmp(present_precision_tier_name(PresentPrecisionTier::PresentWait2), "VF3-present-wait2") == 0);
    assert(std::strcmp(present_precision_tier_name(PresentPrecisionTier::PresentTiming), "VF4-present-timing") == 0);
    return 0;
}
