#include "openxr_timeline.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>

using namespace xrflex;

static policy::CanonicalFrameToken render(std::uint64_t epoch, std::uint64_t sequence) {
    policy::CanonicalFrameToken token{};
    token.epoch = epoch;
    token.sequence = sequence;
    token.transport = policy::FrameTransport::NativeVulkan;
    return token;
}

int main() {
    XrFlexTimeline timeline;
    constexpr std::uintptr_t session = 0x12340000u;

    // xrWaitFrame owns throttling and creates an XR-domain token. The render
    // watermark is the last render known before this new XR frame starts.
    auto w1 = timeline.on_wait(session, 1'000'000, 11'111'111, true, 100, 40);
    assert(w1.has_value());
    assert(w1->sequence == 1);
    assert(w1->predicted_display_time == 1'000'000);
    assert(w1->predicted_display_period == 11'111'111);
    assert(w1->should_render);
    assert(!w1->begun && !w1->ended);

    auto b1 = timeline.on_begin(session, false, 110);
    assert(b1.has_value());
    assert(b1->sequence == 1);
    assert(b1->begun);

    // OpenXR explicitly permits xrWaitFrame on a different thread and lets the
    // next wait unblock once the previous frame was begun, before xrEndFrame.
    auto w2 = timeline.on_wait(session, 2'000'000, 11'111'111, true, 120, 40);
    assert(w2.has_value());
    assert(w2->sequence == 2);

    // A render that is not newer than the wait watermark is intentionally not
    // correlated: it belongs to the previous graphics frame.
    auto e1 = timeline.on_end(session, 1'000'000, 130, render(7, 40));
    assert(e1.has_value());
    assert(e1->ended);
    assert(e1->render_sequence == 0);

    auto b2 = timeline.on_begin(session, false, 140);
    assert(b2.has_value());
    assert(b2->sequence == 2);

    auto e2 = timeline.on_end(session, 2'000'000, 150, render(7, 41));
    assert(e2.has_value());
    assert(e2->render_epoch == 7);
    assert(e2->render_sequence == 41);

    // XR_FRAME_DISCARDED means the previously begun frame is forfeited and the
    // current begin still starts the next waited frame.
    auto w3 = timeline.on_wait(session, 3'000'000, 11'111'111, false, 160, 41);
    auto b3 = timeline.on_begin(session, false, 170);
    assert(w3 && b3);
    auto w4 = timeline.on_wait(session, 4'000'000, 11'111'111, true, 180, 41);
    auto b4 = timeline.on_begin(session, true, 190);
    assert(w4 && b4);
    assert(b4->sequence == 4);
    assert(!b4->discarded);

    // The discarded frame remains visible in stats while the new frame can end
    // normally and correlate to the next render token.
    auto e4 = timeline.on_end(session, 4'000'000, 200, render(7, 42));
    assert(e4 && e4->render_sequence == 42);

    // Independent sessions use independent frame sequences and epochs.
    constexpr std::uintptr_t session2 = 0x99990000u;
    auto w_other = timeline.on_wait(session2, 5'000'000, 8'333'333, true, 210, 0);
    assert(w_other && w_other->sequence == 1);

    const auto stats = timeline.stats();
    assert(stats.waits == 5);
    assert(stats.begins == 4);
    assert(stats.ends == 3);
    assert(stats.discarded == 1);
    assert(stats.correlated == 2);
    assert(stats.clocked_waits == 0);
    assert(stats.clocked_ends == 0);
    assert(stats.dropped == 0);

    const auto old_epoch = w_other->epoch;
    timeline.on_destroy_session(session2);
    assert(!timeline.latest(session2).has_value());
    const auto w_reused = timeline.on_wait(session2, 6'000'000, 8'333'333, true, 220, 0);
    assert(w_reused && w_reused->sequence == 1);
    assert(w_reused->epoch != old_epoch);

    // Probe-chain holes must not hide a colliding live session. Both keys map
    // to the same start slot; destroying the first must leave the second found.
    XrFlexTimeline collisions;
    constexpr std::uintptr_t collision_a = 0x1000u;
    constexpr std::uintptr_t collision_b = 0x1100u; // same (session >> 4) % 16
    assert(collisions.on_wait(collision_a, 1, 1, true, 1, 0));
    assert(collisions.on_wait(collision_b, 2, 1, true, 2, 0));
    collisions.on_destroy_session(collision_a);
    const auto still_live = collisions.latest(collision_b);
    assert(still_live && still_live->predicted_display_time == 2);

    // A pipelined renderer can advance several canonical frames between wait
    // and end. Correlate to the first frame after the wait watermark, not the
    // newest frame visible at xrEndFrame.
    XrFlexTimeline pipeline;
    constexpr std::uintptr_t pipeline_session = 0x77770000u;
    assert(pipeline.on_wait(pipeline_session, 10, 1, true, 1, 100, std::nullopt, 9));
    assert(pipeline.on_begin(pipeline_session, false, 2));
    const auto pipelined_end = pipeline.on_end(pipeline_session, 10, 3, render(9, 103));
    assert(pipelined_end && pipelined_end->render_sequence == 101);

    // shouldRender=false frames must not inherit a graphics token.
    assert(pipeline.on_wait(pipeline_session, 11, 1, false, 4, 103, std::nullopt, 9));
    assert(pipeline.on_begin(pipeline_session, false, 5));
    const auto no_render_end = pipeline.on_end(pipeline_session, 11, 6, render(9, 104));
    assert(no_render_end && no_render_end->render_sequence == 0);

    // Crossing a canonical epoch invalidates the wait watermark correlation.
    assert(pipeline.on_wait(pipeline_session, 12, 1, true, 7, 104, std::nullopt, 9));
    assert(pipeline.on_begin(pipeline_session, false, 8));
    const auto epoch_mismatch = pipeline.on_end(pipeline_session, 12, 9, render(10, 105));
    assert(epoch_mismatch && epoch_mismatch->render_sequence == 0);

    // If observation attaches with no previous canonical watermark, bind to the
    // actually observed token rather than fabricating canonical sequence 1.
    XrFlexTimeline late_attach;
    constexpr std::uintptr_t attach_session = 0x88880000u;
    assert(late_attach.on_wait(attach_session, 13, 1, true, 10, 0));
    assert(late_attach.on_begin(attach_session, false, 11));
    const auto attached_end = late_attach.on_end(attach_session, 13, 12, render(11, 77));
    assert(attached_end && attached_end->render_sequence == 77);

    std::cout << "openxr timeline test: PASS\n";
    return 0;
}
