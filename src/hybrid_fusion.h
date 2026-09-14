#pragma once

#include "input_arbiter.h"
#include "runtime_policy.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace policy {

enum HybridControlField : std::uint8_t {
    HybridControlEnabled = 1u << 0,
    HybridControlBoost = 1u << 1,
    HybridControlMinimumInterval = 1u << 2,
    HybridControlMarkers = 1u << 3,
};

enum class FrameTransport : std::uint8_t {
    Unknown,
    D3D12,
    NativeVulkan,
    Vkd3dD3D12,
    DxvkD3D11,
};

enum class HybridAspect : std::uint8_t {
    Pacing,
    Enabled,
    Boost,
    MinimumInterval,
    MarkerOptimization,
    SimulationLifecycle,
    RenderLifecycle,
    PresentLifecycle,
    InputSampling,
    TriggerFlash,
    PcLatencyPing,
    OutOfBandLifecycle,
    FrameGeneration,
    Count,
};

struct HybridControlPacket {
    bool enabled = false;
    bool boost = false;
    std::uint32_t minimum_interval_us = 0;
    bool use_markers_to_optimize = false;
    std::uint8_t valid_fields = 0;
};

struct HybridResolvedControl {
    HybridControlPacket value{};
    InputFrontend pacing_source = InputFrontend::Auto;
    InputFrontend enabled_source = InputFrontend::Auto;
    InputFrontend boost_source = InputFrontend::Auto;
    InputFrontend interval_source = InputFrontend::Auto;
    InputFrontend markers_source = InputFrontend::Auto;
    std::uint64_t signature = 0;
};

// A transport-neutral frame identity. Native frontend IDs are deliberately not
// exposed as the canonical execution ID: Reflex, XeLL, Anti-Lag 2, Vulkan
// present IDs and frame-generation presents are independent domains. The
// monotonic sequence is the only ID sent to the execution backend.
struct CanonicalFrameToken {
    std::uint64_t epoch = 0;
    // R3.8: sequence is explicitly the render-timeline identity. It remains
    // named `sequence` for source compatibility with R3.4-R3.7 callers.
    std::uint64_t sequence = 0;
    InputFrontend pacing_source = InputFrontend::Auto;
    FrameTransport transport = FrameTransport::Unknown;
    std::uint64_t source_frame_id = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return epoch != 0 && sequence != 0;
    }
};

// Presentation is an independent clock domain. Frame generation makes the
// relationship render->present 1:N, so a presentation token carries both its
// own monotonic sequence and the render token that produced it. `generation`
// is 1 for the first presentation associated with a render frame, 2 for the
// second, and so on; it does not assume a specific FG multiplier.
struct CanonicalPresentationToken {
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    std::uint64_t render_sequence = 0;
    std::uint32_t generation = 0;
    bool interpolated = false;
    InputFrontend source = InputFrontend::Auto;
    FrameTransport transport = FrameTransport::Unknown;
    std::uint64_t source_frame_id = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return epoch != 0 && sequence != 0 && render_sequence != 0 && generation != 0;
    }
};

class HybridFusion {
public:
    static constexpr std::size_t kFrontendCount = 4;
    static constexpr std::size_t kFrameSlots = 64;
    static constexpr std::size_t kPresentationSlots = 128;
    static constexpr std::size_t kAspectCount = static_cast<std::size_t>(HybridAspect::Count);

    void reset() noexcept;
    void set_transport(FrameTransport transport) noexcept {
        transport_.store(static_cast<std::uint8_t>(transport), std::memory_order_release);
    }
    void publish_control(InputFrontend frontend, const HybridControlPacket& packet) noexcept;
    void publish_pacing_observation(InputFrontend frontend, std::uint64_t frame_id = 0) noexcept;

    // Returns an internal monotonically increasing FrameToken only when the
    // startup-locked pacing owner is known. Repeated calls for the same native
    // frontend frame resolve to the same token without allocation or scanning.
    [[nodiscard]] std::optional<CanonicalFrameToken> canonicalize_pacing_observation(
        InputFrontend frontend, std::uint64_t frame_id) noexcept;

    [[nodiscard]] bool locked() const noexcept {
        return lock_state_.load(std::memory_order_acquire) == 2;
    }

    [[nodiscard]] std::optional<InputFrontend> source_for_aspect(HybridAspect aspect) const noexcept {
        return locked_source(aspect);
    }

    [[nodiscard]] std::optional<InputFrontend> resolve_pacing_source(
        const RuntimePolicySnapshot& snapshot,
        const InputArbiter& arbiter,
        std::uint8_t eligible_mask) noexcept;

    [[nodiscard]] std::optional<HybridResolvedControl> resolve_control(
        const RuntimePolicySnapshot& snapshot,
        const InputArbiter& arbiter,
        std::uint8_t eligible_mask) noexcept;

    [[nodiscard]] bool accept_marker(
        InputFrontend frontend,
        NormalizedMarker marker,
        std::uint64_t frame_id,
        const RuntimePolicySnapshot& snapshot,
        const InputArbiter& arbiter,
        std::uint8_t eligible_mask) noexcept;

    // Steady-state fast path after startup ownership has been frozen. No policy
    // snapshot, scorer, freshness check or dynamic allocation is touched.
    [[nodiscard]] bool accept_marker_locked(
        InputFrontend frontend,
        NormalizedMarker marker,
        std::uint64_t frame_id) noexcept;

    [[nodiscard]] std::optional<CanonicalFrameToken> canonicalize_marker(
        InputFrontend frontend,
        NormalizedMarker marker,
        std::uint64_t frame_id,
        const RuntimePolicySnapshot& snapshot,
        const InputArbiter& arbiter,
        std::uint8_t eligible_mask) noexcept;

    [[nodiscard]] std::optional<CanonicalFrameToken> canonicalize_marker_locked(
        InputFrontend frontend,
        NormalizedMarker marker,
        std::uint64_t frame_id) noexcept;

    [[nodiscard]] bool publish_fg_type(
        InputFrontend frontend,
        std::uint64_t frame_id,
        bool interpolated,
        const RuntimePolicySnapshot& snapshot,
        const InputArbiter& arbiter,
        std::uint8_t eligible_mask) noexcept;

    [[nodiscard]] bool publish_fg_type_locked(
        InputFrontend frontend,
        std::uint64_t frame_id,
        bool interpolated) noexcept;

    [[nodiscard]] std::optional<CanonicalPresentationToken> canonicalize_fg_presentation(
        InputFrontend frontend,
        std::uint64_t frame_id,
        bool interpolated,
        const RuntimePolicySnapshot& snapshot,
        const InputArbiter& arbiter,
        std::uint8_t eligible_mask) noexcept;

    [[nodiscard]] std::optional<CanonicalPresentationToken> canonicalize_fg_presentation_locked(
        InputFrontend frontend,
        std::uint64_t frame_id,
        bool interpolated) noexcept;

    [[nodiscard]] std::optional<CanonicalPresentationToken> lookup_presentation(
        std::uint64_t epoch, std::uint64_t sequence) const noexcept;

    // R4.1 OpenXR correlation watermark/source. This is observe-only: callers
    // can correlate an XR frame with a render token that appeared after
    // xrWaitFrame without creating a new render token or changing pacing.
    [[nodiscard]] std::optional<CanonicalFrameToken> latest_canonical_frame() const noexcept;

private:
    struct ControlState {
        // Entire control packet, including valid_fields, is published as one
        // coherent 64-bit snapshot. This prevents a reader from combining the
        // field mask from one update with values from another.
        std::atomic<std::uint64_t> packed{0};
    };

    struct FrameState {
        std::atomic<std::uint64_t> token_sequence{0};
        std::atomic<std::uint64_t> token_epoch{0};
        std::atomic<std::uint8_t> token_transport{static_cast<std::uint8_t>(FrameTransport::Unknown)};
        std::array<std::atomic<std::uint64_t>, kFrontendCount> source_frame_ids{};
        std::atomic<std::uint16_t> marker_mask{0};
        std::atomic<std::uint8_t> pacing_source{static_cast<std::uint8_t>(InputFrontend::Auto)};
        std::atomic<std::uint8_t> fg_flags{0};
        std::atomic<std::uint8_t> fg_source{static_cast<std::uint8_t>(InputFrontend::Auto)};
        std::atomic<std::uint32_t> presentation_count{0};
        std::atomic<std::uint64_t> latest_presentation_sequence{0};
    };

    struct PresentationState {
        // presentation_sequence is the publication word; fields are populated
        // first and made visible with a final release store.
        std::atomic<std::uint64_t> presentation_sequence{0};
        std::atomic<std::uint64_t> epoch{0};
        std::atomic<std::uint64_t> render_sequence{0};
        std::atomic<std::uint32_t> generation{0};
        std::atomic<std::uint8_t> flags{0};
        std::atomic<std::uint8_t> source{static_cast<std::uint8_t>(InputFrontend::Auto)};
        std::atomic<std::uint8_t> transport{static_cast<std::uint8_t>(FrameTransport::Unknown)};
        std::atomic<std::uint64_t> source_frame_id{0};
    };

    struct SourceBinding {
        // native_frame_id is the publication word. Readers load it with acquire
        // before consuming epoch/sequence. Writers clear it before reusing a
        // modulo slot and publish it last with release.
        std::atomic<std::uint64_t> native_frame_id{0};
        std::atomic<std::uint64_t> token_epoch{0};
        std::atomic<std::uint64_t> token_sequence{0};
    };

    std::array<ControlState, kFrontendCount> controls_{};
    std::array<FrameState, kFrameSlots> frames_{};
    std::array<PresentationState, kPresentationSlots> presentations_{};
    std::array<std::array<SourceBinding, kFrameSlots>, kFrontendCount> source_bindings_{};
    std::array<std::atomic<std::uint8_t>, kAspectCount> aspect_owner_{};
    // Read-optimized frozen owner table: 4 bits per HybridAspect. It is built
    // once during startup freeze and then consumed by the marker/pacing/FG hot
    // paths with a single cacheline read. lock_state_==2 is the publication
    // barrier for this word.
    std::atomic<std::uint64_t> frozen_owner_word_{0};
    std::atomic<std::uint8_t> pacing_seen_mask_{0};
    std::atomic<std::uint8_t> fg_seen_mask_{0};
    std::atomic<std::uint64_t> startup_begin_sequence_{0};
    std::atomic<std::uint8_t> transport_{static_cast<std::uint8_t>(FrameTransport::Unknown)};
    std::atomic<std::uint64_t> timeline_epoch_{1};
    std::atomic<std::uint64_t> next_token_sequence_{0};
    std::atomic<std::uint64_t> latest_token_sequence_{0};
    std::atomic<std::uint64_t> next_presentation_sequence_{0};
    std::array<std::atomic<std::uint64_t>, kFrontendCount> last_source_token_{};
    std::atomic_flag token_publish_gate_ = ATOMIC_FLAG_INIT;
    std::atomic_flag presentation_publish_gate_ = ATOMIC_FLAG_INIT;
    // 0 = collecting startup evidence, 1 = one thread is freezing owners, 2 = locked.
    std::atomic<std::uint8_t> lock_state_{0};

    [[nodiscard]] static std::optional<std::size_t> index_of(InputFrontend frontend) noexcept;
    [[nodiscard]] static std::uint8_t priority_of(
        const RuntimePolicySnapshot& snapshot, InputFrontend frontend) noexcept;
    [[nodiscard]] static std::size_t aspect_index(HybridAspect aspect) noexcept;
    [[nodiscard]] static HybridAspect aspect_for_marker(NormalizedMarker marker) noexcept;
    [[nodiscard]] static std::uint8_t semantic_bias(InputFrontend frontend, HybridAspect aspect) noexcept;
    [[nodiscard]] static std::uint16_t marker_bit(NormalizedMarker marker) noexcept;
    [[nodiscard]] static std::uint16_t aspect_marker_mask(HybridAspect aspect) noexcept;
    [[nodiscard]] static std::uint64_t pack_control(const HybridControlPacket& packet) noexcept;
    [[nodiscard]] static HybridControlPacket unpack_control(std::uint64_t packed) noexcept;
    [[nodiscard]] static std::uint64_t pack_owner_word(
        const std::array<std::atomic<std::uint8_t>, kAspectCount>& owners) noexcept;
    [[nodiscard]] static InputFrontend owner_from_word(
        std::uint64_t word, HybridAspect aspect) noexcept;
    [[nodiscard]] static std::uint64_t control_signature(const HybridResolvedControl& resolved) noexcept;

    [[nodiscard]] bool candidate_eligible(
        InputFrontend frontend,
        HybridAspect aspect,
        std::uint8_t required_control_field,
        const RuntimePolicySnapshot& snapshot,
        const InputArbiter& arbiter,
        std::uint8_t eligible_mask,
        bool require_startup_evidence) const noexcept;

    [[nodiscard]] std::uint16_t candidate_score(
        InputFrontend frontend,
        HybridAspect aspect,
        const InputArbiter& arbiter) const noexcept;

    [[nodiscard]] std::optional<InputFrontend> choose_best_source(
        HybridAspect aspect,
        std::uint8_t required_control_field,
        const RuntimePolicySnapshot& snapshot,
        const InputArbiter& arbiter,
        std::uint8_t eligible_mask,
        bool require_startup_evidence) const noexcept;

    [[nodiscard]] bool maybe_lock_startup(
        const RuntimePolicySnapshot& snapshot,
        const InputArbiter& arbiter,
        std::uint8_t eligible_mask) noexcept;

    [[nodiscard]] std::optional<InputFrontend> locked_source(HybridAspect aspect) const noexcept;
    [[nodiscard]] HybridControlPacket packet_for(InputFrontend frontend) const noexcept;
    [[nodiscard]] CanonicalFrameToken token_from_frame(
        const FrameState& frame, InputFrontend source, std::uint64_t source_frame_id) const noexcept;
    [[nodiscard]] FrameState* frame_for_token(std::uint64_t epoch, std::uint64_t sequence) noexcept;
    [[nodiscard]] FrameState* lookup_source_frame(InputFrontend source, std::uint64_t frame_id) noexcept;
    [[nodiscard]] CanonicalPresentationToken token_from_presentation(
        const PresentationState& presentation) const noexcept;
    void publish_source_binding(
        InputFrontend source, std::uint64_t frame_id, const FrameState& frame) noexcept;
    [[nodiscard]] FrameState* bind_source_to_latest(
        InputFrontend source, std::uint64_t frame_id) noexcept;
    FrameState* establish_canonical_frame(InputFrontend pacing_source, std::uint64_t frame_id) noexcept;
};

} // namespace policy
