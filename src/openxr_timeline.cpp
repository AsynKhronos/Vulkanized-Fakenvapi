#include "openxr_timeline.h"
#include "clock_math.h"

#include <utility>

namespace xrflex {

namespace {
inline void serial_increment(std::atomic<std::uint64_t>& counter) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
}

void XrFlexTimeline::reset() noexcept {
    for (auto& session : sessions_) {
        session.ready.store(false, std::memory_order_release);
        session.key.store(0, std::memory_order_release);
        session.epoch.fetch_add(1, std::memory_order_acq_rel);
        session.next_sequence.store(0, std::memory_order_relaxed);
        session.next_begin_sequence.store(0, std::memory_order_relaxed);
        session.active_begin_sequence.store(0, std::memory_order_relaxed);
        session.latest_sequence.store(0, std::memory_order_relaxed);
        session.waits.store(0, std::memory_order_relaxed);
        session.begins.store(0, std::memory_order_relaxed);
        session.ends.store(0, std::memory_order_relaxed);
        session.discarded.store(0, std::memory_order_relaxed);
        session.correlated.store(0, std::memory_order_relaxed);
        session.clocked_waits.store(0, std::memory_order_relaxed);
        session.clocked_ends.store(0, std::memory_order_relaxed);
        for (auto& frame : session.frames) {
            frame.revision.store(0, std::memory_order_relaxed);
            frame.sequence.store(0, std::memory_order_release);
        }
    }
    waits_.store(0, std::memory_order_relaxed);
    begins_.store(0, std::memory_order_relaxed);
    ends_.store(0, std::memory_order_relaxed);
    discarded_.store(0, std::memory_order_relaxed);
    correlated_.store(0, std::memory_order_relaxed);
    clocked_waits_.store(0, std::memory_order_relaxed);
    clocked_ends_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
}

XrFlexTimeline::SessionState* XrFlexTimeline::find_or_claim(std::uintptr_t session) noexcept {
    if (session == 0) return nullptr;
    if (auto* existing = find(session)) return existing;

    const auto start = (session >> 4) % kSessionSlots;
    for (std::size_t probe = 0; probe < kSessionSlots; ++probe) {
        auto& slot = sessions_[(start + probe) % kSessionSlots];
        const auto current = slot.key.load(std::memory_order_acquire);
        if (current == session) {
            // Another valid caller cannot race the first xrWaitFrame for a
            // session. If instrumentation nevertheless catches publication in
            // progress, drop this sample rather than exposing stale slot data.
            return slot.ready.load(std::memory_order_acquire) ? &slot : nullptr;
        }
        if (current != 0) continue;

        auto expected = std::uintptr_t{0};
        if (!slot.key.compare_exchange_strong(
                expected, session,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (expected == session)
                return slot.ready.load(std::memory_order_acquire) ? &slot : nullptr;
            continue;
        }

        // on_destroy_session clears ready before releasing key=0, so a newly
        // claimed slot always starts unreadable. Initialize the complete state
        // and publish ready only once every field is reset.
        slot.ready.store(false, std::memory_order_relaxed);
        slot.epoch.fetch_add(1, std::memory_order_acq_rel);
        slot.next_sequence.store(0, std::memory_order_relaxed);
        slot.next_begin_sequence.store(0, std::memory_order_relaxed);
        slot.active_begin_sequence.store(0, std::memory_order_relaxed);
        slot.latest_sequence.store(0, std::memory_order_relaxed);
        slot.waits.store(0, std::memory_order_relaxed);
        slot.begins.store(0, std::memory_order_relaxed);
        slot.ends.store(0, std::memory_order_relaxed);
        slot.discarded.store(0, std::memory_order_relaxed);
        slot.correlated.store(0, std::memory_order_relaxed);
        slot.clocked_waits.store(0, std::memory_order_relaxed);
        slot.clocked_ends.store(0, std::memory_order_relaxed);
        for (auto& frame : slot.frames) {
            frame.revision.store(0, std::memory_order_relaxed);
            frame.sequence.store(0, std::memory_order_release);
        }
        slot.ready.store(true, std::memory_order_release);
        return &slot;
    }
    return nullptr;
}

XrFlexTimeline::SessionState* XrFlexTimeline::find(std::uintptr_t session) noexcept {
    return const_cast<SessionState*>(std::as_const(*this).find(session));
}

const XrFlexTimeline::SessionState* XrFlexTimeline::find(std::uintptr_t session) const noexcept {
    if (session == 0) return nullptr;

    // xrWaitFrame may live on a different thread from begin/end, but each side
    // repeatedly touches the same session. A TLS cache reduces the normal path
    // to publication-key + ready validation without changing table ownership.
    struct ThreadCache {
        const XrFlexTimeline* owner = nullptr;
        std::uintptr_t session = 0;
        const SessionState* state = nullptr;
    };
    static thread_local ThreadCache cache;
    if (cache.owner == this && cache.session == session && cache.state &&
        cache.state->key.load(std::memory_order_acquire) == session &&
        cache.state->ready.load(std::memory_order_acquire))
        return cache.state;

    const auto start = (session >> 4) % kSessionSlots;
    for (std::size_t probe = 0; probe < kSessionSlots; ++probe) {
        const auto& slot = sessions_[(start + probe) % kSessionSlots];
        if (slot.key.load(std::memory_order_acquire) != session) continue;
        if (!slot.ready.load(std::memory_order_acquire)) return nullptr;
        cache = {this, session, &slot};
        return &slot;
    }
    return nullptr;
}

XrFlexTimeline::FrameState* XrFlexTimeline::frame_for(
    SessionState& state, std::uint64_t sequence) noexcept {
    if (sequence == 0 || sequence == kBusy) return nullptr;
    auto& frame = state.frames[sequence % kFramesPerSession];
    if (frame.sequence.load(std::memory_order_acquire) != sequence) return nullptr;
    return &frame;
}

const XrFlexTimeline::FrameState* XrFlexTimeline::frame_for(
    const SessionState& state, std::uint64_t sequence) noexcept {
    if (sequence == 0 || sequence == kBusy) return nullptr;
    const auto& frame = state.frames[sequence % kFramesPerSession];
    if (frame.sequence.load(std::memory_order_acquire) != sequence) return nullptr;
    return &frame;
}

XrFrameToken XrFlexTimeline::token_from(
    std::uintptr_t session, const FrameState& frame) noexcept {
    XrFrameToken token{};
    const auto sequence = frame.sequence.load(std::memory_order_acquire);
    if (sequence == 0 || sequence == kBusy) return token;
    const auto revision = frame.revision.load(std::memory_order_acquire);
    if ((revision & 1u) != 0) return token;

    token.sequence = sequence;
    token.session = session;
    token.epoch = frame.epoch.load(std::memory_order_relaxed);
    token.predicted_display_time = frame.predicted_display_time.load(std::memory_order_relaxed);
    token.predicted_display_period = frame.predicted_display_period.load(std::memory_order_relaxed);
    token.submitted_display_time = frame.submitted_display_time.load(std::memory_order_relaxed);
    token.wait_return_ns = frame.wait_return_ns.load(std::memory_order_relaxed);
    token.begin_return_ns = frame.begin_return_ns.load(std::memory_order_relaxed);
    token.end_return_ns = frame.end_return_ns.load(std::memory_order_relaxed);
    token.predicted_display_host_ns = frame.predicted_display_host_ns.load(std::memory_order_relaxed);
    token.submitted_display_host_ns = frame.submitted_display_host_ns.load(std::memory_order_relaxed);
    token.wait_to_display_ns = frame.wait_to_display_ns.load(std::memory_order_relaxed);
    token.end_to_display_ns = frame.end_to_display_ns.load(std::memory_order_relaxed);
    token.render_epoch = frame.render_epoch.load(std::memory_order_relaxed);
    token.render_sequence = frame.render_sequence.load(std::memory_order_relaxed);
    const auto flags = frame.flags.load(std::memory_order_relaxed);
    token.should_render = (flags & ShouldRender) != 0;
    token.begun = (flags & Begun) != 0;
    token.ended = (flags & Ended) != 0;
    token.discarded = (flags & Discarded) != 0;
    token.predicted_clock_valid = (flags & PredictedClockValid) != 0;
    token.submitted_clock_valid = (flags & SubmittedClockValid) != 0;

    // A frame slot is reused modulo kFramesPerSession and begin/end can update
    // an already published wait record. Revalidate both identity and revision so
    // readers never combine fields from two generations or update phases.
    if (frame.revision.load(std::memory_order_acquire) != revision ||
        frame.sequence.load(std::memory_order_acquire) != sequence)
        return {};
    return token;
}

std::optional<XrFrameToken> XrFlexTimeline::on_wait(
    std::uintptr_t session,
    std::int64_t predicted_display_time,
    std::int64_t predicted_display_period,
    bool should_render,
    std::uint64_t wait_return_ns,
    std::uint64_t render_sequence_watermark,
    std::optional<std::uint64_t> predicted_display_host_ns,
    std::uint64_t render_epoch_watermark) noexcept {
    auto* state = find_or_claim(session);
    if (!state) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    // xrWaitFrame calls for one XrSession are externally synchronized by the
    // OpenXR contract, so this counter has exactly one writer at a time. Keep
    // it atomic for thread migration/observation, but avoid a locked RMW.
    const auto sequence = state->next_sequence.load(std::memory_order_relaxed) + 1;
    state->next_sequence.store(sequence, std::memory_order_relaxed);
    auto& frame = state->frames[sequence % kFramesPerSession];
    frame.sequence.store(kBusy, std::memory_order_release);
    frame.revision.store(0, std::memory_order_relaxed);
    frame.epoch.store(state->epoch.load(std::memory_order_acquire), std::memory_order_relaxed);
    frame.predicted_display_time.store(predicted_display_time, std::memory_order_relaxed);
    frame.predicted_display_period.store(predicted_display_period, std::memory_order_relaxed);
    frame.submitted_display_time.store(0, std::memory_order_relaxed);
    frame.wait_return_ns.store(wait_return_ns, std::memory_order_relaxed);
    frame.begin_return_ns.store(0, std::memory_order_relaxed);
    frame.end_return_ns.store(0, std::memory_order_relaxed);
    frame.predicted_display_host_ns.store(predicted_display_host_ns.value_or(0), std::memory_order_relaxed);
    frame.submitted_display_host_ns.store(0, std::memory_order_relaxed);
    frame.wait_to_display_ns.store(
        predicted_display_host_ns
            ? hostclock::signed_delta_ns(*predicted_display_host_ns, wait_return_ns)
            : 0,
        std::memory_order_relaxed);
    frame.end_to_display_ns.store(0, std::memory_order_relaxed);
    frame.render_watermark.store(render_sequence_watermark, std::memory_order_relaxed);
    frame.render_watermark_epoch.store(render_epoch_watermark, std::memory_order_relaxed);
    frame.render_epoch.store(0, std::memory_order_relaxed);
    frame.render_sequence.store(0, std::memory_order_relaxed);
    std::uint8_t wait_flags = should_render ? ShouldRender : 0;
    if (predicted_display_host_ns) {
        wait_flags |= PredictedClockValid;
        serial_increment(state->clocked_waits);
    }
    frame.flags.store(wait_flags, std::memory_order_relaxed);
    frame.sequence.store(sequence, std::memory_order_release);
    state->latest_sequence.store(sequence, std::memory_order_release);
    serial_increment(state->waits);
    return token_from(session, frame);
}

std::optional<XrFrameToken> XrFlexTimeline::on_begin(
    std::uintptr_t session,
    bool discarded_previous,
    std::uint64_t begin_return_ns) noexcept {
    auto* state = find(session);
    if (!state) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    // xrBeginFrame/xrEndFrame access to a session is externally synchronized.
    // active_begin_sequence therefore needs atomic visibility, not an x86 LOCKed
    // exchange. The same rule lets frame revisions use store publication.
    if (discarded_previous) {
        const auto previous = state->active_begin_sequence.load(std::memory_order_acquire);
        state->active_begin_sequence.store(0, std::memory_order_release);
        if (auto* old = frame_for(*state, previous)) {
            const auto revision = old->revision.load(std::memory_order_relaxed);
            old->revision.store(revision + 1, std::memory_order_release);
            const auto flags = old->flags.load(std::memory_order_relaxed);
            old->flags.store(static_cast<std::uint8_t>(flags | Discarded), std::memory_order_relaxed);
            old->revision.store(revision + 2, std::memory_order_release);
            serial_increment(state->discarded);
        }
    }

    const auto sequence = state->next_begin_sequence.load(std::memory_order_relaxed) + 1;
    state->next_begin_sequence.store(sequence, std::memory_order_relaxed);
    auto* frame = frame_for(*state, sequence);
    if (!frame) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    const auto revision = frame->revision.load(std::memory_order_relaxed);
    frame->revision.store(revision + 1, std::memory_order_release);
    frame->begin_return_ns.store(begin_return_ns, std::memory_order_relaxed);
    const auto flags = frame->flags.load(std::memory_order_relaxed);
    frame->flags.store(static_cast<std::uint8_t>(flags | Begun), std::memory_order_relaxed);
    frame->revision.store(revision + 2, std::memory_order_release);
    state->active_begin_sequence.store(sequence, std::memory_order_release);
    serial_increment(state->begins);
    const auto token = token_from(session, *frame);
    return token ? std::optional<XrFrameToken>{token} : std::nullopt;
}

std::optional<XrFrameToken> XrFlexTimeline::on_end(
    std::uintptr_t session,
    std::int64_t submitted_display_time,
    std::uint64_t end_return_ns,
    const std::optional<policy::CanonicalFrameToken>& latest_render,
    std::optional<std::uint64_t> submitted_display_host_ns) noexcept {
    auto* state = find(session);
    if (!state) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    const auto sequence = state->active_begin_sequence.load(std::memory_order_acquire);
    state->active_begin_sequence.store(0, std::memory_order_release);
    auto* frame = frame_for(*state, sequence);
    if (!frame) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    const auto revision = frame->revision.load(std::memory_order_relaxed);
    frame->revision.store(revision + 1, std::memory_order_release);
    frame->submitted_display_time.store(submitted_display_time, std::memory_order_relaxed);
    frame->end_return_ns.store(end_return_ns, std::memory_order_relaxed);
    frame->submitted_display_host_ns.store(submitted_display_host_ns.value_or(0), std::memory_order_relaxed);

    auto flags = frame->flags.load(std::memory_order_relaxed);
    if (submitted_display_host_ns) {
        frame->end_to_display_ns.store(
            hostclock::signed_delta_ns(*submitted_display_host_ns, end_return_ns),
            std::memory_order_relaxed);
        flags = static_cast<std::uint8_t>(flags | SubmittedClockValid);
        serial_increment(state->clocked_ends);
    }

    // Correlate to the first canonical render token that appeared after this
    // xrWaitFrame watermark, not blindly to the newest token at xrEndFrame. The
    // latter can be one or more frames ahead in a pipelined renderer. A frame
    // for which the runtime said shouldRender=false must never inherit an
    // unrelated graphics token.
    const auto watermark = frame->render_watermark.load(std::memory_order_relaxed);
    const auto watermark_epoch = frame->render_watermark_epoch.load(std::memory_order_relaxed);
    const bool render_expected = (flags & ShouldRender) != 0;
    if (render_expected && latest_render && latest_render->sequence > watermark &&
        (watermark == 0 || watermark_epoch == 0 || latest_render->epoch == watermark_epoch)) {
        // With a watermark, canonical frame sequences are monotone and the
        // first graphics token after xrWaitFrame is watermark+1 even if
        // xrEndFrame observes a newer pipelined frame. If XRFlex attached with
        // no prior watermark, we cannot reconstruct history and therefore bind
        // to the actually observed latest token rather than invent sequence 1.
        const auto correlated_sequence = watermark != 0
            ? watermark + 1
            : latest_render->sequence;
        frame->render_epoch.store(latest_render->epoch, std::memory_order_relaxed);
        frame->render_sequence.store(correlated_sequence, std::memory_order_relaxed);
        serial_increment(state->correlated);
    }

    flags = static_cast<std::uint8_t>(flags | Ended);
    frame->flags.store(flags, std::memory_order_relaxed);
    frame->revision.store(revision + 2, std::memory_order_release);
    serial_increment(state->ends);
    const auto token = token_from(session, *frame);
    return token ? std::optional<XrFrameToken>{token} : std::nullopt;
}

void XrFlexTimeline::on_destroy_session(std::uintptr_t session) noexcept {
    auto* state = find(session);
    if (!state) return;

    // Flush session-local counters only at the cold lifetime boundary.
    waits_.fetch_add(state->waits.load(std::memory_order_relaxed), std::memory_order_relaxed);
    begins_.fetch_add(state->begins.load(std::memory_order_relaxed), std::memory_order_relaxed);
    ends_.fetch_add(state->ends.load(std::memory_order_relaxed), std::memory_order_relaxed);
    discarded_.fetch_add(state->discarded.load(std::memory_order_relaxed), std::memory_order_relaxed);
    correlated_.fetch_add(state->correlated.load(std::memory_order_relaxed), std::memory_order_relaxed);
    clocked_waits_.fetch_add(state->clocked_waits.load(std::memory_order_relaxed), std::memory_order_relaxed);
    clocked_ends_.fetch_add(state->clocked_ends.load(std::memory_order_relaxed), std::memory_order_relaxed);

    state->ready.store(false, std::memory_order_release);
    state->epoch.fetch_add(1, std::memory_order_acq_rel);
    state->active_begin_sequence.store(0, std::memory_order_relaxed);
    state->latest_sequence.store(0, std::memory_order_relaxed);
    state->key.store(0, std::memory_order_release);
}

std::optional<XrFrameToken> XrFlexTimeline::latest(std::uintptr_t session) const noexcept {
    const auto* state = find(session);
    if (!state) return std::nullopt;
    const auto sequence = state->latest_sequence.load(std::memory_order_acquire);
    const auto* frame = frame_for(*state, sequence);
    if (!frame) return std::nullopt;
    const auto token = token_from(session, *frame);
    return token ? std::optional<XrFrameToken>{token} : std::nullopt;
}

TimelineStats XrFlexTimeline::stats() const noexcept {
    TimelineStats result{};
    result.waits = waits_.load(std::memory_order_relaxed);
    result.begins = begins_.load(std::memory_order_relaxed);
    result.ends = ends_.load(std::memory_order_relaxed);
    result.discarded = discarded_.load(std::memory_order_relaxed);
    result.correlated = correlated_.load(std::memory_order_relaxed);
    result.clocked_waits = clocked_waits_.load(std::memory_order_relaxed);
    result.clocked_ends = clocked_ends_.load(std::memory_order_relaxed);
    result.dropped = dropped_.load(std::memory_order_relaxed);

    for (const auto& state : sessions_) {
        if (state.key.load(std::memory_order_acquire) == 0 ||
            !state.ready.load(std::memory_order_acquire))
            continue;
        result.waits += state.waits.load(std::memory_order_relaxed);
        result.begins += state.begins.load(std::memory_order_relaxed);
        result.ends += state.ends.load(std::memory_order_relaxed);
        result.discarded += state.discarded.load(std::memory_order_relaxed);
        result.correlated += state.correlated.load(std::memory_order_relaxed);
        result.clocked_waits += state.clocked_waits.load(std::memory_order_relaxed);
        result.clocked_ends += state.clocked_ends.load(std::memory_order_relaxed);
    }
    return result;
}

} // namespace xrflex
