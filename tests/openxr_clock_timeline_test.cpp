#include "openxr_timeline.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace xrflex;

int main() {
    XrFlexTimeline timeline;
    constexpr std::uintptr_t session = 0xabc000u;

    // Runtime-predicted XrTime has already been converted by the observer to
    // the canonical QPC-nanosecond host domain before entering the timeline.
    auto wait = timeline.on_wait(
        session,
        9'000'000,
        11'111'111,
        true,
        1'000'000'000ull,
        0,
        1'008'000'000ull);
    assert(wait);
    assert(wait->predicted_clock_valid);
    assert(wait->predicted_display_host_ns == 1'008'000'000ull);
    assert(wait->wait_to_display_ns == 8'000'000);

    auto begin = timeline.on_begin(session, false, 1'001'000'000ull);
    assert(begin);

    // xrEndFrame can return after the requested display time. Preserve the
    // sign so telemetry can distinguish positive headroom from a missed edge.
    auto end = timeline.on_end(
        session,
        9'000'000,
        1'009'500'000ull,
        std::nullopt,
        1'008'000'000ull);
    assert(end);
    assert(end->submitted_clock_valid);
    assert(end->submitted_display_host_ns == 1'008'000'000ull);
    assert(end->end_to_display_ns == -1'500'000);

    const auto stats = timeline.stats();
    assert(stats.clocked_waits == 1);
    assert(stats.clocked_ends == 1);

    // Missing conversion capability must remain a valid observer path with
    // zero canonical-clock fields rather than guessing an XrTime offset.
    constexpr std::uintptr_t session2 = 0xdef000u;
    auto no_clock = timeline.on_wait(session2, 10'000, 8'333'333, true, 50, 0);
    assert(no_clock);
    assert(!no_clock->predicted_clock_valid);
    assert(no_clock->predicted_display_host_ns == 0);
    assert(no_clock->wait_to_display_ns == 0);

    std::cout << "openxr canonical clock timeline test: PASS\n";
    return 0;
}
