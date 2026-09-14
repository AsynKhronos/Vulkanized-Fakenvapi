#include "input_arbiter.h"

#include <algorithm>

namespace policy {

std::optional<std::size_t> InputArbiter::index_of(InputFrontend frontend) noexcept {
    switch (frontend) {
        case InputFrontend::Reflex: return 0;
        case InputFrontend::VkNvLowLatency2: return 1;
        case InputFrontend::XeLL: return 2;
        case InputFrontend::AntiLag2: return 3;
        case InputFrontend::Auto: return std::nullopt;
    }
    return std::nullopt;
}

std::uint8_t InputArbiter::priority_of(
    const RuntimePolicySnapshot& snapshot,
    InputFrontend frontend) noexcept {
    for (std::uint8_t i = 0; i < snapshot.input.priority.size; ++i) {
        if (snapshot.input.priority.values[i] == frontend) return i;
    }
    return 0xff;
}

void InputArbiter::reset() noexcept {
    observation_sequence_.store(0, std::memory_order_relaxed);
    for (auto& state : state_) {
        state.last_frame_id.store(0, std::memory_order_relaxed);
        state.last_observation_sequence.store(0, std::memory_order_relaxed);
        state.marker_mask.store(0, std::memory_order_relaxed);
        state.evidence_mask.store(0, std::memory_order_relaxed);
        state.quality.store(0, std::memory_order_relaxed);
        state.seen.store(false, std::memory_order_relaxed);
    }
}

void InputArbiter::note_evidence(InputFrontend frontend, InputEvidence evidence) noexcept {
    const auto index = index_of(frontend);
    if (!index.has_value() || evidence == InputEvidenceNone) return;

    auto& state = state_[*index];
    const auto bit = static_cast<std::uint16_t>(evidence);

    // Evidence is monotonic for a session. After the first publication this is
    // the overwhelmingly common path, so avoid paying a locked fetch_or for
    // every marker/control callback just to rediscover an already-set bit.
    if ((state.evidence_mask.load(std::memory_order_relaxed) & bit) != 0) return;
    const auto previous = state.evidence_mask.fetch_or(bit, std::memory_order_relaxed);
    if ((previous & bit) != 0) return;

    std::uint8_t bonus = 0;
    switch (evidence) {
        case InputEvidenceCapability:  bonus = 2; break;
        case InputEvidenceContext:     bonus = 8; break;
        case InputEvidenceControl:     bonus = 14; break;
        case InputEvidenceSleep:       bonus = 18; break;
        case InputEvidenceMarker:      bonus = 4; break;
        case InputEvidenceAsyncMarker: bonus = 8; break;
        case InputEvidenceFrameGen:    bonus = 10; break;
        case InputEvidenceNone: break;
    }

    auto current = state.quality.load(std::memory_order_relaxed);
    while (true) {
        const auto desired = static_cast<std::uint8_t>(std::min<int>(100, current + bonus));
        if (state.quality.compare_exchange_weak(
                current, desired, std::memory_order_release, std::memory_order_relaxed)) {
            break;
        }
    }
}

void InputArbiter::observe(InputFrontend frontend, NormalizedMarker marker, std::uint64_t frame_id) noexcept {
    const auto index = index_of(frontend);
    if (!index.has_value()) return;

    note_evidence(frontend, InputEvidenceMarker);

    auto& state = state_[*index];

    // seen is write-once until reset. Keep the steady path read-only and only
    // perform the locked exchange on the first observation (or a true race for
    // that first observation).
    bool was_seen = state.seen.load(std::memory_order_relaxed);
    if (!was_seen)
        was_seen = state.seen.exchange(true, std::memory_order_relaxed);

    // Freshness is frame-granular, not callback-granular. A frontend that emits
    // six markers for one native frame must not age a one-marker frontend six
    // times faster. Only the thread that actually publishes a different native
    // frame id advances the shared activity sequence. Same-frame markers are
    // therefore free of the global locked fetch_add.
    std::uint64_t previous_frame = state.last_frame_id.load(std::memory_order_relaxed);
    bool frame_advanced = !was_seen;
    if (previous_frame != frame_id) {
        previous_frame = state.last_frame_id.exchange(frame_id, std::memory_order_relaxed);
        frame_advanced = frame_advanced || previous_frame != frame_id;
    }

    if (frame_advanced) {
        const auto sequence = observation_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        state.last_observation_sequence.store(sequence, std::memory_order_release);
    }

    const auto prior_quality = state.quality.load(std::memory_order_relaxed);
    std::uint8_t score = was_seen ? prior_quality : static_cast<std::uint8_t>(std::max<int>(20, prior_quality));

    if (was_seen) {
        if (frame_id > previous_frame) {
            score = static_cast<std::uint8_t>(std::min<int>(100, score + 4));
        } else if (frame_id < previous_frame) {
            score = static_cast<std::uint8_t>(score > 12 ? score - 12 : 0);
        }
    }

    // Reliability grows once per *distinct native frame*, not once per callback.
    // The forward-progress bonus above is deliberately the only recurring
    // quality increment; marker vocabulary contributes only on first
    // observation. This prevents a verbose frontend from winning arbitration
    // merely by emitting more callbacks for the same native frame.

    const auto marker_bit = static_cast<std::uint16_t>(1u << (static_cast<unsigned>(marker) & 0x0f));
    auto previous_mask = state.marker_mask.load(std::memory_order_relaxed);
    if ((previous_mask & marker_bit) == 0)
        previous_mask = state.marker_mask.fetch_or(marker_bit, std::memory_order_relaxed);
    const bool newly_observed_marker = (previous_mask & marker_bit) == 0;
    if (newly_observed_marker) {
        score = static_cast<std::uint8_t>(std::min<int>(100, score + 8));

        switch (marker) {
            case NormalizedMarker::SimulationStart:
            case NormalizedMarker::PresentStart:
            case NormalizedMarker::RenderSubmitStart:
            case NormalizedMarker::InputSample:
                score = static_cast<std::uint8_t>(std::min<int>(100, score + 5));
                break;
            default:
                break;
        }
    }

    state.quality.store(score, std::memory_order_release);
}

std::uint8_t InputArbiter::quality(InputFrontend frontend) const noexcept {
    const auto index = index_of(frontend);
    return index.has_value() ? state_[*index].quality.load(std::memory_order_acquire) : 0;
}

std::uint64_t InputArbiter::last_frame_id(InputFrontend frontend) const noexcept {
    const auto index = index_of(frontend);
    return index.has_value() ? state_[*index].last_frame_id.load(std::memory_order_acquire) : 0;
}

std::uint16_t InputArbiter::observed_marker_mask(InputFrontend frontend) const noexcept {
    const auto index = index_of(frontend);
    return index.has_value() ? state_[*index].marker_mask.load(std::memory_order_acquire) : 0;
}

std::uint16_t InputArbiter::evidence_mask(InputFrontend frontend) const noexcept {
    const auto index = index_of(frontend);
    return index.has_value() ? state_[*index].evidence_mask.load(std::memory_order_acquire) : 0;
}

InputRecognition InputArbiter::recognition(InputFrontend frontend) const noexcept {
    InputRecognition result{};
    const auto index = index_of(frontend);
    if (!index.has_value()) return result;

    const auto& state = state_[*index];
    result.last_frame_id = state.last_frame_id.load(std::memory_order_acquire);
    result.last_observation_sequence = state.last_observation_sequence.load(std::memory_order_acquire);
    result.marker_mask = state.marker_mask.load(std::memory_order_acquire);
    result.evidence_mask = state.evidence_mask.load(std::memory_order_acquire);
    result.quality = state.quality.load(std::memory_order_acquire);
    result.seen = state.seen.load(std::memory_order_acquire);
    result.fresh = fresh(frontend);
    return result;
}

std::uint64_t InputArbiter::observation_sequence() const noexcept {
    return observation_sequence_.load(std::memory_order_acquire);
}

bool InputArbiter::fresh(InputFrontend frontend) const noexcept {
    const auto index = index_of(frontend);
    if (!index.has_value()) return false;

    const auto& state = state_[*index];
    if (!state.seen.load(std::memory_order_acquire)) return false;

    const std::uint64_t now = observation_sequence_.load(std::memory_order_acquire);
    const std::uint64_t last = state.last_observation_sequence.load(std::memory_order_acquire);
    return now >= last && (now - last) <= kStaleObservationWindow;
}

std::optional<InputFrontend> InputArbiter::select(
    const RuntimePolicySnapshot& snapshot,
    InputFrontend current,
    std::uint8_t eligible_mask) const noexcept {
    // Freeze the observation watermark once for this decision. The previous
    // implementation reloaded it through fresh() for every candidate and then
    // reloaded quality again for ranking. A single watermark both reduces
    // cache traffic and makes one arbitration decision internally coherent.
    const auto now = observation_sequence_.load(std::memory_order_acquire);

    const auto candidate_quality = [&](InputFrontend frontend, std::uint8_t* quality_out) noexcept {
        const auto index = index_of(frontend);
        if (!index.has_value()) return false;
        if ((eligible_mask & static_cast<std::uint8_t>(1u << *index)) == 0) return false;

        const auto& state = state_[*index];
        if (!state.seen.load(std::memory_order_relaxed)) return false;
        const auto last = state.last_observation_sequence.load(std::memory_order_acquire);
        if (now < last || (now - last) > kStaleObservationWindow) return false;

        const auto q = state.quality.load(std::memory_order_acquire);
        if (q < snapshot.input.minimum_quality) return false;
        if (quality_out) *quality_out = q;
        return true;
    };

    if (snapshot.input.mode != InputFrontend::Auto) {
        if (candidate_quality(snapshot.input.mode, nullptr)) return snapshot.input.mode;
        if (!snapshot.general.allow_fallback) return std::nullopt;
    }

    std::optional<InputFrontend> best;
    std::uint8_t best_quality = 0;
    std::uint8_t best_priority = 0xff;

    for (std::uint8_t priority = 0; priority < snapshot.input.priority.size; ++priority) {
        const InputFrontend frontend = snapshot.input.priority.values[priority];
        std::uint8_t q = 0;
        if (!candidate_quality(frontend, &q)) continue;

        if (!best.has_value() || q > best_quality ||
            (q == best_quality && priority < best_priority)) {
            best = frontend;
            best_quality = q;
            best_priority = priority;
        }
    }

    // Once an incumbent is healthy, keep it unless a challenger is materially
    // better. Priority breaks ties only when there is no healthy incumbent;
    // an equal-quality callback must never displace an already selected source.
    const std::uint8_t current_priority = priority_of(snapshot, current);
    if (current != InputFrontend::Auto && current_priority != 0xff) {
        std::uint8_t current_quality = 0;
        if (candidate_quality(current, &current_quality)) {
            if (!best.has_value() || *best == current) return current;
            if (best_quality < static_cast<unsigned>(current_quality) + kSwitchQualityMargin)
                return current;
        }
    }

    return best;
}

} // namespace policy
