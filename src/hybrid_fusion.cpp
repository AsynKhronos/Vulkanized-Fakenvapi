#include "hybrid_fusion.h"

#include <algorithm>
#include <limits>

namespace policy {
namespace {

constexpr std::uint64_t kEnabled = 1ull << 0;
constexpr std::uint64_t kBoost = 1ull << 1;
constexpr std::uint64_t kMarkers = 1ull << 2;
constexpr unsigned kValidFieldsShift = 8;
constexpr std::uint64_t kValidFieldsMask = 0xffull << kValidFieldsShift;
constexpr unsigned kOwnerBits = 4;
constexpr std::uint64_t kOwnerMask = (1ull << kOwnerBits) - 1ull;
constexpr std::uint64_t kBusyFrame = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint8_t kFgKnown = 1u << 0;
constexpr std::uint8_t kFgInterpolated = 1u << 1;

} // namespace

std::optional<std::size_t> HybridFusion::index_of(InputFrontend frontend) noexcept {
    switch (frontend) {
        case InputFrontend::Reflex: return 0;
        case InputFrontend::VkNvLowLatency2: return 1;
        case InputFrontend::XeLL: return 2;
        case InputFrontend::AntiLag2: return 3;
        case InputFrontend::Auto: return std::nullopt;
    }
    return std::nullopt;
}

std::uint8_t HybridFusion::priority_of(
    const RuntimePolicySnapshot& snapshot, InputFrontend frontend) noexcept {
    for (std::uint8_t i = 0; i < snapshot.input.priority.size; ++i) {
        if (snapshot.input.priority.values[i] == frontend) return i;
    }
    return 0xff;
}

std::size_t HybridFusion::aspect_index(HybridAspect aspect) noexcept {
    return static_cast<std::size_t>(aspect);
}

HybridAspect HybridFusion::aspect_for_marker(NormalizedMarker marker) noexcept {
    switch (marker) {
        case NormalizedMarker::SimulationStart:
        case NormalizedMarker::SimulationEnd:
            return HybridAspect::SimulationLifecycle;
        case NormalizedMarker::RenderSubmitStart:
        case NormalizedMarker::RenderSubmitEnd:
            return HybridAspect::RenderLifecycle;
        case NormalizedMarker::PresentStart:
        case NormalizedMarker::PresentEnd:
            return HybridAspect::PresentLifecycle;
        case NormalizedMarker::InputSample:
            return HybridAspect::InputSampling;
        case NormalizedMarker::TriggerFlash:
            return HybridAspect::TriggerFlash;
        case NormalizedMarker::PcLatencyPing:
            return HybridAspect::PcLatencyPing;
        case NormalizedMarker::OutOfBandRenderSubmitStart:
        case NormalizedMarker::OutOfBandRenderSubmitEnd:
        case NormalizedMarker::OutOfBandPresentStart:
        case NormalizedMarker::OutOfBandPresentEnd:
            return HybridAspect::OutOfBandLifecycle;
    }
    return HybridAspect::SimulationLifecycle;
}

std::uint8_t HybridFusion::semantic_bias(InputFrontend frontend, HybridAspect aspect) noexcept {
    switch (aspect) {
        case HybridAspect::Pacing:
            switch (frontend) {
                case InputFrontend::AntiLag2: return 12;
                case InputFrontend::Reflex: return 10;
                case InputFrontend::VkNvLowLatency2: return 10;
                case InputFrontend::XeLL: return 10;
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::Enabled:
            switch (frontend) {
                case InputFrontend::Reflex: return 14;
                case InputFrontend::VkNvLowLatency2: return 14;
                case InputFrontend::XeLL: return 13;
                case InputFrontend::AntiLag2: return 11;
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::Boost:
            switch (frontend) {
                case InputFrontend::Reflex: return 16;
                case InputFrontend::VkNvLowLatency2: return 16;
                case InputFrontend::XeLL: return 14;
                case InputFrontend::AntiLag2:
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::MinimumInterval:
            switch (frontend) {
                case InputFrontend::XeLL: return 16;
                case InputFrontend::Reflex: return 14;
                case InputFrontend::VkNvLowLatency2: return 14;
                case InputFrontend::AntiLag2: return 8;
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::MarkerOptimization:
            switch (frontend) {
                case InputFrontend::Reflex: return 16;
                case InputFrontend::VkNvLowLatency2: return 16;
                case InputFrontend::XeLL:
                case InputFrontend::AntiLag2:
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::SimulationLifecycle:
        case HybridAspect::RenderLifecycle:
            switch (frontend) {
                case InputFrontend::Reflex: return 16;
                case InputFrontend::VkNvLowLatency2: return 16;
                case InputFrontend::XeLL: return 14;
                case InputFrontend::AntiLag2: return 8;
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::PresentLifecycle:
            switch (frontend) {
                case InputFrontend::Reflex: return 14;
                case InputFrontend::VkNvLowLatency2: return 14;
                case InputFrontend::XeLL: return 14;
                case InputFrontend::AntiLag2: return 8;
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::InputSampling:
            switch (frontend) {
                case InputFrontend::AntiLag2: return 16;
                case InputFrontend::XeLL: return 14;
                case InputFrontend::Reflex: return 10;
                case InputFrontend::VkNvLowLatency2: return 10;
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::TriggerFlash:
        case HybridAspect::PcLatencyPing:
            switch (frontend) {
                case InputFrontend::Reflex: return 18;
                case InputFrontend::VkNvLowLatency2: return 16;
                case InputFrontend::XeLL:
                case InputFrontend::AntiLag2:
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::OutOfBandLifecycle:
            switch (frontend) {
                case InputFrontend::Reflex: return 16;
                case InputFrontend::VkNvLowLatency2: return 16;
                case InputFrontend::XeLL: return 6;
                case InputFrontend::AntiLag2: return 4;
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::FrameGeneration:
            switch (frontend) {
                case InputFrontend::AntiLag2: return 18;
                case InputFrontend::Reflex: return 6;
                case InputFrontend::VkNvLowLatency2: return 6;
                case InputFrontend::XeLL: return 4;
                case InputFrontend::Auto: return 0;
            }
            break;
        case HybridAspect::Count:
            break;
    }
    return 0;
}

std::uint16_t HybridFusion::marker_bit(NormalizedMarker marker) noexcept {
    return static_cast<std::uint16_t>(1u << (static_cast<unsigned>(marker) & 0x0f));
}

std::uint16_t HybridFusion::aspect_marker_mask(HybridAspect aspect) noexcept {
    const auto bit = [](NormalizedMarker marker) {
        return static_cast<std::uint16_t>(1u << (static_cast<unsigned>(marker) & 0x0f));
    };
    switch (aspect) {
        case HybridAspect::SimulationLifecycle:
            return bit(NormalizedMarker::SimulationStart) | bit(NormalizedMarker::SimulationEnd);
        case HybridAspect::RenderLifecycle:
            return bit(NormalizedMarker::RenderSubmitStart) | bit(NormalizedMarker::RenderSubmitEnd);
        case HybridAspect::PresentLifecycle:
            return bit(NormalizedMarker::PresentStart) | bit(NormalizedMarker::PresentEnd);
        case HybridAspect::InputSampling:
            return bit(NormalizedMarker::InputSample);
        case HybridAspect::TriggerFlash:
            return bit(NormalizedMarker::TriggerFlash);
        case HybridAspect::PcLatencyPing:
            return bit(NormalizedMarker::PcLatencyPing);
        case HybridAspect::OutOfBandLifecycle:
            return bit(NormalizedMarker::OutOfBandRenderSubmitStart) |
                   bit(NormalizedMarker::OutOfBandRenderSubmitEnd) |
                   bit(NormalizedMarker::OutOfBandPresentStart) |
                   bit(NormalizedMarker::OutOfBandPresentEnd);
        default:
            return 0;
    }
}

std::uint64_t HybridFusion::pack_control(const HybridControlPacket& packet) noexcept {
    std::uint64_t packed = 0;
    if (packet.enabled) packed |= kEnabled;
    if (packet.boost) packed |= kBoost;
    if (packet.use_markers_to_optimize) packed |= kMarkers;
    packed |= (static_cast<std::uint64_t>(packet.valid_fields) << kValidFieldsShift) & kValidFieldsMask;
    packed |= static_cast<std::uint64_t>(packet.minimum_interval_us) << 32;
    return packed;
}

HybridControlPacket HybridFusion::unpack_control(std::uint64_t packed) noexcept {
    HybridControlPacket packet{};
    packet.enabled = (packed & kEnabled) != 0;
    packet.boost = (packed & kBoost) != 0;
    packet.use_markers_to_optimize = (packed & kMarkers) != 0;
    packet.minimum_interval_us = static_cast<std::uint32_t>(packed >> 32);
    packet.valid_fields = static_cast<std::uint8_t>((packed & kValidFieldsMask) >> kValidFieldsShift);
    return packet;
}

std::uint64_t HybridFusion::pack_owner_word(
    const std::array<std::atomic<std::uint8_t>, kAspectCount>& owners) noexcept {
    static_assert(kAspectCount * kOwnerBits <= 64);
    std::uint64_t word = 0;
    for (std::size_t i = 0; i < kAspectCount; ++i) {
        const auto owner = static_cast<std::uint64_t>(owners[i].load(std::memory_order_relaxed));
        word |= (owner & kOwnerMask) << (i * kOwnerBits);
    }
    return word;
}

InputFrontend HybridFusion::owner_from_word(std::uint64_t word, HybridAspect aspect) noexcept {
    const auto shift = aspect_index(aspect) * kOwnerBits;
    return static_cast<InputFrontend>((word >> shift) & kOwnerMask);
}

void HybridFusion::reset() noexcept {
    for (auto& control : controls_)
        control.packed.store(0, std::memory_order_relaxed);
    for (auto& frame : frames_) {
        frame.token_sequence.store(0, std::memory_order_relaxed);
        frame.token_epoch.store(0, std::memory_order_relaxed);
        frame.token_transport.store(static_cast<std::uint8_t>(FrameTransport::Unknown), std::memory_order_relaxed);
        for (auto& source_id : frame.source_frame_ids)
            source_id.store(0, std::memory_order_relaxed);
        frame.marker_mask.store(0, std::memory_order_relaxed);
        frame.pacing_source.store(static_cast<std::uint8_t>(InputFrontend::Auto), std::memory_order_relaxed);
        frame.fg_flags.store(0, std::memory_order_relaxed);
        frame.fg_source.store(static_cast<std::uint8_t>(InputFrontend::Auto), std::memory_order_relaxed);
        frame.presentation_count.store(0, std::memory_order_relaxed);
        frame.latest_presentation_sequence.store(0, std::memory_order_relaxed);
    }
    for (auto& presentation : presentations_) {
        presentation.presentation_sequence.store(0, std::memory_order_relaxed);
        presentation.epoch.store(0, std::memory_order_relaxed);
        presentation.render_sequence.store(0, std::memory_order_relaxed);
        presentation.generation.store(0, std::memory_order_relaxed);
        presentation.flags.store(0, std::memory_order_relaxed);
        presentation.source.store(static_cast<std::uint8_t>(InputFrontend::Auto), std::memory_order_relaxed);
        presentation.transport.store(static_cast<std::uint8_t>(FrameTransport::Unknown), std::memory_order_relaxed);
        presentation.source_frame_id.store(0, std::memory_order_relaxed);
    }
    for (auto& source : source_bindings_) {
        for (auto& binding : source) {
            binding.native_frame_id.store(0, std::memory_order_relaxed);
            binding.token_epoch.store(0, std::memory_order_relaxed);
            binding.token_sequence.store(0, std::memory_order_relaxed);
        }
    }
    for (auto& last : last_source_token_)
        last.store(0, std::memory_order_relaxed);
    for (auto& owner : aspect_owner_) {
        owner.store(static_cast<std::uint8_t>(InputFrontend::Auto), std::memory_order_relaxed);
    }
    frozen_owner_word_.store(0, std::memory_order_relaxed);
    pacing_seen_mask_.store(0, std::memory_order_relaxed);
    fg_seen_mask_.store(0, std::memory_order_relaxed);
    startup_begin_sequence_.store(0, std::memory_order_relaxed);
    transport_.store(static_cast<std::uint8_t>(FrameTransport::Unknown), std::memory_order_relaxed);
    next_token_sequence_.store(0, std::memory_order_relaxed);
    latest_token_sequence_.store(0, std::memory_order_relaxed);
    next_presentation_sequence_.store(0, std::memory_order_relaxed);
    timeline_epoch_.fetch_add(1, std::memory_order_acq_rel);
    token_publish_gate_.clear(std::memory_order_release);
    presentation_publish_gate_.clear(std::memory_order_release);
    lock_state_.store(0, std::memory_order_relaxed);
}

void HybridFusion::publish_control(InputFrontend frontend, const HybridControlPacket& packet) noexcept {
    const auto index = index_of(frontend);
    if (!index.has_value()) return;
    controls_[*index].packed.store(pack_control(packet), std::memory_order_release);
}

void HybridFusion::publish_pacing_observation(
    InputFrontend frontend, std::uint64_t frame_id) noexcept {
    const auto index = index_of(frontend);
    if (!index.has_value()) return;

    const auto lock_state = lock_state_.load(std::memory_order_acquire);
    if (lock_state != 2) {
        const auto bit = static_cast<std::uint8_t>(1u << *index);
        const auto seen = pacing_seen_mask_.load(std::memory_order_relaxed);
        if ((seen & bit) == 0)
            pacing_seen_mask_.fetch_or(bit, std::memory_order_release);
        return;
    }

    (void)canonicalize_pacing_observation(frontend, frame_id);
}

std::optional<CanonicalFrameToken> HybridFusion::canonicalize_pacing_observation(
    InputFrontend frontend, std::uint64_t frame_id) noexcept {
    if (frontend == InputFrontend::Auto || frame_id == 0 || frame_id == kBusyFrame ||
        lock_state_.load(std::memory_order_acquire) != 2) {
        return std::nullopt;
    }

    const auto owners = frozen_owner_word_.load(std::memory_order_relaxed);
    const auto pacing = owner_from_word(owners, HybridAspect::Pacing);
    if (pacing != frontend) return std::nullopt;

    auto* frame = establish_canonical_frame(frontend, frame_id);
    if (!frame) return std::nullopt;
    return token_from_frame(*frame, frontend, frame_id);
}

HybridControlPacket HybridFusion::packet_for(InputFrontend frontend) const noexcept {
    const auto index = index_of(frontend);
    if (!index.has_value()) return {};
    return unpack_control(controls_[*index].packed.load(std::memory_order_acquire));
}

bool HybridFusion::candidate_eligible(
    InputFrontend frontend,
    HybridAspect aspect,
    std::uint8_t required_control_field,
    const RuntimePolicySnapshot& snapshot,
    const InputArbiter& arbiter,
    std::uint8_t eligible_mask,
    bool require_startup_evidence) const noexcept {
    const auto index = index_of(frontend);
    if (!index.has_value()) return false;
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << *index);
    if ((eligible_mask & bit) == 0) return false;
    if (!arbiter.fresh(frontend)) return false;
    if (arbiter.quality(frontend) < snapshot.hybrid.aspect_minimum_quality) return false;
    if (semantic_bias(frontend, aspect) == 0) return false;

    if (required_control_field != 0) {
        const auto packet = unpack_control(controls_[*index].packed.load(std::memory_order_acquire));
        if ((packet.valid_fields & required_control_field) == 0) return false;
    }

    if (!require_startup_evidence) return true;

    if (aspect == HybridAspect::Pacing) {
        return (pacing_seen_mask_.load(std::memory_order_acquire) & bit) != 0;
    }
    if (aspect == HybridAspect::FrameGeneration) {
        return (fg_seen_mask_.load(std::memory_order_acquire) & bit) != 0;
    }

    const auto required_markers = aspect_marker_mask(aspect);
    if (required_markers != 0) {
        const auto observed = arbiter.observed_marker_mask(frontend);
        // Lifecycle ownership is frozen for the complete semantic set. A
        // frontend that only exposed Start (or only End) during startup must
        // not permanently own both callbacks.
        return (observed & required_markers) == required_markers;
    }
    return true;
}

std::uint16_t HybridFusion::candidate_score(
    InputFrontend frontend,
    HybridAspect aspect,
    const InputArbiter& arbiter) const noexcept {
    return static_cast<std::uint16_t>(arbiter.quality(frontend)) + semantic_bias(frontend, aspect);
}

std::optional<InputFrontend> HybridFusion::choose_best_source(
    HybridAspect aspect,
    std::uint8_t required_control_field,
    const RuntimePolicySnapshot& snapshot,
    const InputArbiter& arbiter,
    std::uint8_t eligible_mask,
    bool require_startup_evidence) const noexcept {
    std::optional<InputFrontend> best;
    std::uint16_t best_score = 0;
    std::uint8_t best_priority = 0xff;

    for (std::uint8_t priority = 0; priority < snapshot.input.priority.size; ++priority) {
        const InputFrontend candidate = snapshot.input.priority.values[priority];
        if (!candidate_eligible(candidate, aspect, required_control_field,
                                snapshot, arbiter, eligible_mask, require_startup_evidence)) {
            continue;
        }
        const auto score = candidate_score(candidate, aspect, arbiter);
        if (!best.has_value() || score > best_score ||
            (score == best_score && priority < best_priority)) {
            best = candidate;
            best_score = score;
            best_priority = priority;
        }
    }
    return best;
}

bool HybridFusion::maybe_lock_startup(
    const RuntimePolicySnapshot& snapshot,
    const InputArbiter& arbiter,
    std::uint8_t eligible_mask) noexcept {
    if (locked()) return true;
    if (!snapshot.hybrid.startup_locked) return false;

    const std::uint64_t now = arbiter.observation_sequence();
    if (now == 0) return false;

    bool any = false;
    constexpr InputFrontend frontends[] = {
        InputFrontend::Reflex, InputFrontend::VkNvLowLatency2,
        InputFrontend::XeLL, InputFrontend::AntiLag2,
    };
    for (const auto frontend : frontends) {
        const auto index = index_of(frontend);
        if (!index.has_value()) continue;
        if ((eligible_mask & static_cast<std::uint8_t>(1u << *index)) == 0) continue;
        if (arbiter.fresh(frontend) &&
            arbiter.quality(frontend) >= snapshot.hybrid.aspect_minimum_quality) {
            any = true;
            break;
        }
    }
    if (!any) return false;

    std::uint64_t start = startup_begin_sequence_.load(std::memory_order_acquire);
    if (start == 0) {
        std::uint64_t expected = 0;
        startup_begin_sequence_.compare_exchange_strong(
            expected, now, std::memory_order_acq_rel, std::memory_order_acquire);
        start = startup_begin_sequence_.load(std::memory_order_acquire);
    }

    if (now - start < snapshot.hybrid.startup_observations) return false;

    std::uint8_t expected_state = 0;
    if (!lock_state_.compare_exchange_strong(
            expected_state, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return lock_state_.load(std::memory_order_acquire) == 2;
    }

    struct Requirement { HybridAspect aspect; std::uint8_t control; };
    constexpr Requirement requirements[] = {
        {HybridAspect::Pacing, HybridControlEnabled},
        {HybridAspect::Enabled, HybridControlEnabled},
        {HybridAspect::Boost, HybridControlBoost},
        {HybridAspect::MinimumInterval, HybridControlMinimumInterval},
        {HybridAspect::MarkerOptimization, HybridControlMarkers},
        {HybridAspect::SimulationLifecycle, 0},
        {HybridAspect::RenderLifecycle, 0},
        {HybridAspect::PresentLifecycle, 0},
        {HybridAspect::InputSampling, 0},
        {HybridAspect::TriggerFlash, 0},
        {HybridAspect::PcLatencyPing, 0},
        {HybridAspect::OutOfBandLifecycle, 0},
        {HybridAspect::FrameGeneration, 0},
    };

    for (const auto& requirement : requirements) {
        const auto selected = choose_best_source(
            requirement.aspect, requirement.control, snapshot, arbiter,
            eligible_mask, true);
        aspect_owner_[aspect_index(requirement.aspect)].store(
            static_cast<std::uint8_t>(selected.value_or(InputFrontend::Auto)),
            std::memory_order_release);
    }

    if (static_cast<InputFrontend>(aspect_owner_[aspect_index(HybridAspect::Pacing)].load(
            std::memory_order_acquire)) == InputFrontend::Auto) {
        const auto fallback = choose_best_source(
            HybridAspect::Pacing, HybridControlEnabled,
            snapshot, arbiter, eligible_mask, false);
        aspect_owner_[aspect_index(HybridAspect::Pacing)].store(
            static_cast<std::uint8_t>(fallback.value_or(InputFrontend::Auto)),
            std::memory_order_release);
    }

    frozen_owner_word_.store(pack_owner_word(aspect_owner_), std::memory_order_relaxed);
    lock_state_.store(2, std::memory_order_release);
    return true;
}

std::optional<InputFrontend> HybridFusion::locked_source(HybridAspect aspect) const noexcept {
    if (!locked()) return std::nullopt;
    const auto source = owner_from_word(
        frozen_owner_word_.load(std::memory_order_relaxed), aspect);
    return source == InputFrontend::Auto ? std::nullopt : std::optional<InputFrontend>(source);
}

std::optional<InputFrontend> HybridFusion::resolve_pacing_source(
    const RuntimePolicySnapshot& snapshot,
    const InputArbiter& arbiter,
    std::uint8_t eligible_mask) noexcept {
    if (!maybe_lock_startup(snapshot, arbiter, eligible_mask)) return std::nullopt;
    return locked_source(HybridAspect::Pacing);
}

std::uint64_t HybridFusion::control_signature(const HybridResolvedControl& resolved) noexcept {
    // Collision-free compact layout for the currently representable control
    // surface. Keep this independent from ControlState's publication layout.
    std::uint64_t signature = 0;
    if (resolved.value.enabled) signature |= 1ull << 0;
    if (resolved.value.boost) signature |= 1ull << 1;
    if (resolved.value.use_markers_to_optimize) signature |= 1ull << 2;
    signature |= static_cast<std::uint64_t>(resolved.value.valid_fields & 0x0fu) << 3;
    signature |= static_cast<std::uint64_t>(resolved.pacing_source) << 7;
    signature |= static_cast<std::uint64_t>(resolved.enabled_source) << 10;
    signature |= static_cast<std::uint64_t>(resolved.boost_source) << 13;
    signature |= static_cast<std::uint64_t>(resolved.interval_source) << 16;
    signature |= static_cast<std::uint64_t>(resolved.markers_source) << 19;
    signature |= static_cast<std::uint64_t>(resolved.value.minimum_interval_us) << 32;
    return signature;
}

std::optional<HybridResolvedControl> HybridFusion::resolve_control(
    const RuntimePolicySnapshot& snapshot,
    const InputArbiter& arbiter,
    std::uint8_t eligible_mask) noexcept {
    if (!maybe_lock_startup(snapshot, arbiter, eligible_mask)) return std::nullopt;

    // lock_state_==2 publishes the immutable owner word. Snapshot that routing
    // word and every frontend control packet exactly once so a concurrent game
    // control update cannot mix fields from two generations of one frontend.
    const auto owners = frozen_owner_word_.load(std::memory_order_relaxed);
    const auto pacing = owner_from_word(owners, HybridAspect::Pacing);
    if (pacing == InputFrontend::Auto) return std::nullopt;

    std::array<std::uint64_t, kFrontendCount> control_words{};
    for (std::size_t i = 0; i < kFrontendCount; ++i)
        control_words[i] = controls_[i].packed.load(std::memory_order_acquire);

    const auto packet_from_snapshot = [&](InputFrontend frontend) noexcept {
        const auto index = index_of(frontend);
        return index.has_value() ? unpack_control(control_words[*index]) : HybridControlPacket{};
    };

    HybridResolvedControl resolved{};
    resolved.pacing_source = pacing;

    const auto apply_bool = [&](HybridAspect aspect, std::uint8_t field,
                                bool& value, InputFrontend& source) {
        const auto owner = owner_from_word(owners, aspect);
        if (owner == InputFrontend::Auto) return;
        const auto packet = packet_from_snapshot(owner);
        if ((packet.valid_fields & field) == 0) return;
        source = owner;
        resolved.value.valid_fields |= field;
        if (field == HybridControlEnabled) value = packet.enabled;
        else if (field == HybridControlBoost) value = packet.boost;
        else if (field == HybridControlMarkers) value = packet.use_markers_to_optimize;
    };

    apply_bool(HybridAspect::Enabled, HybridControlEnabled,
               resolved.value.enabled, resolved.enabled_source);
    apply_bool(HybridAspect::Boost, HybridControlBoost,
               resolved.value.boost, resolved.boost_source);
    apply_bool(HybridAspect::MarkerOptimization, HybridControlMarkers,
               resolved.value.use_markers_to_optimize, resolved.markers_source);

    const auto interval_owner = owner_from_word(owners, HybridAspect::MinimumInterval);
    if (interval_owner != InputFrontend::Auto) {
        const auto packet = packet_from_snapshot(interval_owner);
        if ((packet.valid_fields & HybridControlMinimumInterval) != 0) {
            resolved.value.minimum_interval_us = packet.minimum_interval_us;
            resolved.value.valid_fields |= HybridControlMinimumInterval;
            resolved.interval_source = interval_owner;
        }
    }

    if (resolved.enabled_source == InputFrontend::Auto) {
        const auto packet = packet_from_snapshot(pacing);
        if ((packet.valid_fields & HybridControlEnabled) == 0) return std::nullopt;
        resolved.value.enabled = packet.enabled;
        resolved.value.valid_fields |= HybridControlEnabled;
        resolved.enabled_source = pacing;
    }

    resolved.signature = control_signature(resolved);
    return resolved;
}

CanonicalFrameToken HybridFusion::token_from_frame(
    const FrameState& frame, InputFrontend source, std::uint64_t source_frame_id) const noexcept {
    CanonicalFrameToken token{};
    const auto published = frame.token_sequence.load(std::memory_order_acquire);
    if (published == 0 || published == kBusyFrame) return token;
    token.sequence = published;
    token.epoch = frame.token_epoch.load(std::memory_order_relaxed);
    token.pacing_source = static_cast<InputFrontend>(
        frame.pacing_source.load(std::memory_order_relaxed));
    token.transport = static_cast<FrameTransport>(
        frame.token_transport.load(std::memory_order_relaxed));
    token.source_frame_id = source_frame_id;
    if (frame.token_sequence.load(std::memory_order_acquire) != published)
        return {};
    if (source == InputFrontend::Auto) token.source_frame_id = 0;
    return token;
}

HybridFusion::FrameState* HybridFusion::frame_for_token(
    std::uint64_t epoch, std::uint64_t sequence) noexcept {
    if (epoch == 0 || sequence == 0 || sequence == kBusyFrame) return nullptr;
    FrameState& frame = frames_[sequence % kFrameSlots];
    const auto published = frame.token_sequence.load(std::memory_order_acquire);
    if (published != sequence) return nullptr;
    if (frame.token_epoch.load(std::memory_order_relaxed) != epoch) return nullptr;
    // Revalidate the publication word after reading payload. A modulo-slot
    // recycle can otherwise let a delayed reader combine an old sequence with
    // payload already being written for a newer frame.
    if (frame.token_sequence.load(std::memory_order_acquire) != published) return nullptr;
    return &frame;
}

HybridFusion::FrameState* HybridFusion::lookup_source_frame(
    InputFrontend source, std::uint64_t frame_id) noexcept {
    const auto source_index = index_of(source);
    if (!source_index.has_value() || frame_id == 0 || frame_id == kBusyFrame) return nullptr;

    auto& binding = source_bindings_[*source_index][frame_id % kFrameSlots];
    const auto published = binding.native_frame_id.load(std::memory_order_acquire);
    if (published != frame_id) return nullptr;
    const auto epoch = binding.token_epoch.load(std::memory_order_relaxed);
    const auto sequence = binding.token_sequence.load(std::memory_order_relaxed);
    if (binding.native_frame_id.load(std::memory_order_acquire) != published) return nullptr;
    return frame_for_token(epoch, sequence);
}

void HybridFusion::publish_source_binding(
    InputFrontend source, std::uint64_t frame_id, const FrameState& frame) noexcept {
    const auto source_index = index_of(source);
    if (!source_index.has_value() || frame_id == 0 || frame_id == kBusyFrame) return;

    const auto epoch = frame.token_epoch.load(std::memory_order_relaxed);
    const auto sequence = frame.token_sequence.load(std::memory_order_relaxed);
    if (epoch == 0 || sequence == 0 || sequence == kBusyFrame) return;

    auto& binding = source_bindings_[*source_index][frame_id % kFrameSlots];
    // Clear the publication word before reusing this modulo slot. Readers can
    // never combine a new native ID with an old token payload.
    binding.native_frame_id.store(0, std::memory_order_release);
    binding.token_epoch.store(epoch, std::memory_order_relaxed);
    binding.token_sequence.store(sequence, std::memory_order_relaxed);
    binding.native_frame_id.store(frame_id, std::memory_order_release);

    auto observed = last_source_token_[*source_index].load(std::memory_order_relaxed);
    while (observed < sequence &&
           !last_source_token_[*source_index].compare_exchange_weak(
               observed, sequence, std::memory_order_release, std::memory_order_relaxed)) {}
}

HybridFusion::FrameState* HybridFusion::bind_source_to_latest(
    InputFrontend source, std::uint64_t frame_id) noexcept {
    const auto source_index = index_of(source);
    if (!source_index.has_value() || frame_id == 0 || frame_id == kBusyFrame) return nullptr;

    if (auto* existing = lookup_source_frame(source, frame_id)) return existing;

    const auto epoch = timeline_epoch_.load(std::memory_order_acquire);
    const auto latest = latest_token_sequence_.load(std::memory_order_acquire);
    if (latest == 0) return nullptr;

    // Secondary frontend IDs live in independent domains, but each individual
    // domain is monotonic. Bind the common in-order case directly to latest. If
    // the pacing owner is already multiple tokens ahead, use the source's own
    // native-ID delta to recover the exact skipped canonical token. Never attach
    // an ambiguous delayed callback to the newest token merely because it is
    // convenient: dropping one semantic marker is safer than time-shifting it.
    const auto last = last_source_token_[*source_index].load(std::memory_order_acquire);
    if (latest <= last) return nullptr;

    std::uint64_t target = latest;
    const auto gap = latest - last;
    if (last != 0 && gap > 1 && gap < kFrameSlots) {
        auto* previous = frame_for_token(epoch, last);
        if (!previous) return nullptr;
        const auto previous_native = previous->source_frame_ids[*source_index].load(
            std::memory_order_acquire);
        if (previous_native == 0 || frame_id <= previous_native) return nullptr;

        const auto native_delta = frame_id - previous_native;
        if (native_delta == 0 || native_delta > gap) return nullptr;
        target = last + native_delta;
    }
    // If the source was absent for an entire ring window, exact historical
    // correlation no longer exists. Treat its next observation as a fresh
    // resynchronization point at the current frame rather than wedging the
    // source forever.

    auto* frame = frame_for_token(epoch, target);
    if (!frame) return nullptr;

    auto expected = std::uint64_t{0};
    if (!frame->source_frame_ids[*source_index].compare_exchange_strong(
            expected, frame_id, std::memory_order_acq_rel, std::memory_order_acquire)) {
        if (expected != frame_id) return nullptr;
    }

    publish_source_binding(source, frame_id, *frame);
    return frame;
}

HybridFusion::FrameState* HybridFusion::establish_canonical_frame(
    InputFrontend pacing_source,
    std::uint64_t frame_id) noexcept {
    if (frame_id == 0 || frame_id == kBusyFrame) return nullptr;
    const auto source_index = index_of(pacing_source);
    if (!source_index.has_value()) return nullptr;

    if (auto* existing = lookup_source_frame(pacing_source, frame_id)) return existing;

    // Only the frozen pacing source creates FrameTokens. Contention is a
    // pathological duplicate callback; fail open, then retry the O(1) binding
    // lookup in case the winning thread already published the token.
    if (token_publish_gate_.test_and_set(std::memory_order_acquire))
        return lookup_source_frame(pacing_source, frame_id);

    if (auto* existing = lookup_source_frame(pacing_source, frame_id)) {
        token_publish_gate_.clear(std::memory_order_release);
        return existing;
    }

    const auto epoch = timeline_epoch_.load(std::memory_order_acquire);
    const auto sequence = next_token_sequence_.load(std::memory_order_relaxed) + 1;
    next_token_sequence_.store(sequence, std::memory_order_relaxed);
    FrameState& frame = frames_[sequence % kFrameSlots];

    frame.token_sequence.store(kBusyFrame, std::memory_order_release);
    frame.token_epoch.store(epoch, std::memory_order_relaxed);
    frame.token_transport.store(transport_.load(std::memory_order_acquire), std::memory_order_relaxed);
    for (auto& source_id : frame.source_frame_ids)
        source_id.store(0, std::memory_order_relaxed);
    frame.source_frame_ids[*source_index].store(frame_id, std::memory_order_relaxed);
    frame.marker_mask.store(0, std::memory_order_relaxed);
    frame.fg_flags.store(0, std::memory_order_relaxed);
    frame.fg_source.store(static_cast<std::uint8_t>(InputFrontend::Auto), std::memory_order_relaxed);
    frame.presentation_count.store(0, std::memory_order_relaxed);
    frame.latest_presentation_sequence.store(0, std::memory_order_relaxed);
    frame.pacing_source.store(static_cast<std::uint8_t>(pacing_source), std::memory_order_relaxed);
    frame.token_sequence.store(sequence, std::memory_order_release);

    publish_source_binding(pacing_source, frame_id, frame);
    latest_token_sequence_.store(sequence, std::memory_order_release);
    token_publish_gate_.clear(std::memory_order_release);
    return &frame;
}

bool HybridFusion::accept_marker(
    InputFrontend frontend,
    NormalizedMarker marker,
    std::uint64_t frame_id,
    const RuntimePolicySnapshot& snapshot,
    const InputArbiter& arbiter,
    std::uint8_t eligible_mask) noexcept {
    return canonicalize_marker(frontend, marker, frame_id, snapshot, arbiter, eligible_mask).has_value();
}

bool HybridFusion::accept_marker_locked(
    InputFrontend frontend,
    NormalizedMarker marker,
    std::uint64_t frame_id) noexcept {
    return canonicalize_marker_locked(frontend, marker, frame_id).has_value();
}

std::optional<CanonicalFrameToken> HybridFusion::canonicalize_marker(
    InputFrontend frontend,
    NormalizedMarker marker,
    std::uint64_t frame_id,
    const RuntimePolicySnapshot& snapshot,
    const InputArbiter& arbiter,
    std::uint8_t eligible_mask) noexcept {
    if (frontend == InputFrontend::Auto || frame_id == 0) return std::nullopt;
    if (!maybe_lock_startup(snapshot, arbiter, eligible_mask)) return std::nullopt;
    return canonicalize_marker_locked(frontend, marker, frame_id);
}

std::optional<CanonicalFrameToken> HybridFusion::canonicalize_marker_locked(
    InputFrontend frontend,
    NormalizedMarker marker,
    std::uint64_t frame_id) noexcept {
    if (frontend == InputFrontend::Auto || frame_id == 0 ||
        lock_state_.load(std::memory_order_acquire) != 2) {
        return std::nullopt;
    }

    const auto owners = frozen_owner_word_.load(std::memory_order_relaxed);
    const auto pacing = owner_from_word(owners, HybridAspect::Pacing);
    if (pacing == InputFrontend::Auto) return std::nullopt;

    // The pacing owner anchors the FrameToken even when another frontend owns
    // this semantic marker aspect. This preserves the invariant that a Reflex
    // SimulationStart, for example, can attach to an AL2/XeLL pacing frame.
    FrameState* frame = nullptr;
    if (frontend == pacing)
        frame = establish_canonical_frame(pacing, frame_id);

    const auto winner = owner_from_word(owners, aspect_for_marker(marker));
    if (winner != frontend) return std::nullopt;

    if (!frame) {
        frame = lookup_source_frame(frontend, frame_id);
        if (!frame) frame = bind_source_to_latest(frontend, frame_id);
    }
    if (!frame) return std::nullopt;

    const auto bit = marker_bit(marker);
    const auto previous = frame->marker_mask.fetch_or(bit, std::memory_order_acq_rel);
    if ((previous & bit) != 0) return std::nullopt;

    return token_from_frame(*frame, frontend, frame_id);
}

bool HybridFusion::publish_fg_type(
    InputFrontend frontend,
    std::uint64_t frame_id,
    bool interpolated,
    const RuntimePolicySnapshot& snapshot,
    const InputArbiter& arbiter,
    std::uint8_t eligible_mask) noexcept {
    return canonicalize_fg_presentation(
        frontend, frame_id, interpolated, snapshot, arbiter, eligible_mask).has_value();
}

bool HybridFusion::publish_fg_type_locked(
    InputFrontend frontend,
    std::uint64_t frame_id,
    bool interpolated) noexcept {
    return canonicalize_fg_presentation_locked(frontend, frame_id, interpolated).has_value();
}

std::optional<CanonicalPresentationToken> HybridFusion::canonicalize_fg_presentation(
    InputFrontend frontend,
    std::uint64_t frame_id,
    bool interpolated,
    const RuntimePolicySnapshot& snapshot,
    const InputArbiter& arbiter,
    std::uint8_t eligible_mask) noexcept {
    if (frontend == InputFrontend::Auto || frame_id == 0) return std::nullopt;
    const auto index = index_of(frontend);
    if (!index.has_value()) return std::nullopt;

    if (!locked()) {
        const auto bit = static_cast<std::uint8_t>(1u << *index);
        const auto seen = fg_seen_mask_.load(std::memory_order_relaxed);
        if ((seen & bit) == 0)
            fg_seen_mask_.fetch_or(bit, std::memory_order_release);
    }
    if (!maybe_lock_startup(snapshot, arbiter, eligible_mask)) return std::nullopt;
    return canonicalize_fg_presentation_locked(frontend, frame_id, interpolated);
}

CanonicalPresentationToken HybridFusion::token_from_presentation(
    const PresentationState& presentation) const noexcept {
    CanonicalPresentationToken token{};
    // Publication word first: its acquire load synchronizes all payload fields
    // written before the writer's final release store. Revalidate it after the
    // payload so a ring-slot recycle cannot produce a torn presentation token.
    const auto published = presentation.presentation_sequence.load(std::memory_order_acquire);
    if (published == 0 || published == kBusyFrame) return token;
    token.sequence = published;
    token.epoch = presentation.epoch.load(std::memory_order_relaxed);
    token.render_sequence = presentation.render_sequence.load(std::memory_order_relaxed);
    token.generation = presentation.generation.load(std::memory_order_relaxed);
    const auto flags = presentation.flags.load(std::memory_order_relaxed);
    token.interpolated = (flags & kFgInterpolated) != 0;
    token.source = static_cast<InputFrontend>(presentation.source.load(std::memory_order_relaxed));
    token.transport = static_cast<FrameTransport>(presentation.transport.load(std::memory_order_relaxed));
    token.source_frame_id = presentation.source_frame_id.load(std::memory_order_relaxed);
    if (presentation.presentation_sequence.load(std::memory_order_acquire) != published)
        return {};
    return token;
}

std::optional<CanonicalPresentationToken> HybridFusion::lookup_presentation(
    std::uint64_t epoch, std::uint64_t sequence) const noexcept {
    if (epoch == 0 || sequence == 0 || sequence == kBusyFrame) return std::nullopt;
    const auto& presentation = presentations_[sequence % kPresentationSlots];
    if (presentation.presentation_sequence.load(std::memory_order_acquire) != sequence)
        return std::nullopt;
    if (presentation.epoch.load(std::memory_order_relaxed) != epoch)
        return std::nullopt;
    const auto token = token_from_presentation(presentation);
    return token ? std::optional<CanonicalPresentationToken>{token} : std::nullopt;
}

std::optional<CanonicalFrameToken> HybridFusion::latest_canonical_frame() const noexcept {
    const auto sequence = latest_token_sequence_.load(std::memory_order_acquire);
    const auto epoch = timeline_epoch_.load(std::memory_order_acquire);
    if (sequence == 0 || sequence == kBusyFrame || epoch == 0) return std::nullopt;

    const auto& frame = frames_[sequence % kFrameSlots];
    if (frame.token_sequence.load(std::memory_order_acquire) != sequence) return std::nullopt;
    if (frame.token_epoch.load(std::memory_order_relaxed) != epoch) return std::nullopt;

    const auto token = token_from_frame(frame, InputFrontend::Auto, 0);
    return token ? std::optional<CanonicalFrameToken>{token} : std::nullopt;
}

std::optional<CanonicalPresentationToken> HybridFusion::canonicalize_fg_presentation_locked(
    InputFrontend frontend,
    std::uint64_t frame_id,
    bool interpolated) noexcept {
    if (frontend == InputFrontend::Auto || frame_id == 0 ||
        lock_state_.load(std::memory_order_acquire) != 2) {
        return std::nullopt;
    }

    const auto owners = frozen_owner_word_.load(std::memory_order_relaxed);
    const auto winner = owner_from_word(owners, HybridAspect::FrameGeneration);
    if (winner != frontend) return std::nullopt;

    FrameState* frame = lookup_source_frame(frontend, frame_id);
    if (!frame) frame = bind_source_to_latest(frontend, frame_id);
    if (!frame) return std::nullopt;

    // A render frame can legitimately produce more than one presentation.
    // Each FG callback therefore allocates a presentation token rather than
    // mutating the render token into a synthetic one-to-one relationship.
    if (presentation_publish_gate_.test_and_set(std::memory_order_acquire))
        return std::nullopt;

    const auto epoch = frame->token_epoch.load(std::memory_order_acquire);
    const auto render_sequence = frame->token_sequence.load(std::memory_order_acquire);
    if (epoch == 0 || render_sequence == 0 || render_sequence == kBusyFrame) {
        presentation_publish_gate_.clear(std::memory_order_release);
        return std::nullopt;
    }

    const auto presentation_sequence =
        next_presentation_sequence_.load(std::memory_order_relaxed) + 1;
    next_presentation_sequence_.store(presentation_sequence, std::memory_order_relaxed);
    const auto generation = frame->presentation_count.load(std::memory_order_relaxed) + 1;
    frame->presentation_count.store(generation, std::memory_order_relaxed);
    auto& presentation = presentations_[presentation_sequence % kPresentationSlots];

    presentation.presentation_sequence.store(kBusyFrame, std::memory_order_release);
    presentation.epoch.store(epoch, std::memory_order_relaxed);
    presentation.render_sequence.store(render_sequence, std::memory_order_relaxed);
    presentation.generation.store(generation, std::memory_order_relaxed);
    presentation.flags.store(
        static_cast<std::uint8_t>(kFgKnown | (interpolated ? kFgInterpolated : 0)),
        std::memory_order_relaxed);
    presentation.source.store(static_cast<std::uint8_t>(frontend), std::memory_order_relaxed);
    presentation.transport.store(frame->token_transport.load(std::memory_order_relaxed), std::memory_order_relaxed);
    presentation.source_frame_id.store(frame_id, std::memory_order_relaxed);
    presentation.presentation_sequence.store(presentation_sequence, std::memory_order_release);

    frame->fg_source.store(static_cast<std::uint8_t>(frontend), std::memory_order_release);
    frame->fg_flags.store(
        static_cast<std::uint8_t>(kFgKnown | (interpolated ? kFgInterpolated : 0)),
        std::memory_order_release);
    frame->latest_presentation_sequence.store(presentation_sequence, std::memory_order_release);

    const auto token = token_from_presentation(presentation);
    presentation_publish_gate_.clear(std::memory_order_release);
    return token;
}

} // namespace policy
