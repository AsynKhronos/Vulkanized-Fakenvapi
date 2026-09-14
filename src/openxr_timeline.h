#pragma once

#include "hybrid_fusion.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace xrflex {

struct XrFrameToken {
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    std::uintptr_t session = 0;
    std::int64_t predicted_display_time = 0;
    std::int64_t predicted_display_period = 0;
    std::int64_t submitted_display_time = 0;
    std::uint64_t wait_return_ns = 0;
    std::uint64_t begin_return_ns = 0;
    std::uint64_t end_return_ns = 0;
    // R4.2 canonical host clock values. Zero means unavailable. They are
    // obtained by converting the relevant XrTime through the runtime's
    // XR_KHR_win32_convert_performance_counter_time implementation.
    std::uint64_t predicted_display_host_ns = 0;
    std::uint64_t submitted_display_host_ns = 0;
    // Positive slack means the hook returned before the display deadline.
    std::int64_t wait_to_display_ns = 0;
    std::int64_t end_to_display_ns = 0;
    std::uint64_t render_epoch = 0;
    std::uint64_t render_sequence = 0;
    bool should_render = false;
    bool begun = false;
    bool ended = false;
    bool discarded = false;
    bool predicted_clock_valid = false;
    bool submitted_clock_valid = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return epoch != 0 && sequence != 0 && session != 0;
    }
};

struct TimelineStats {
    std::uint64_t waits = 0;
    std::uint64_t begins = 0;
    std::uint64_t ends = 0;
    std::uint64_t discarded = 0;
    std::uint64_t correlated = 0;
    std::uint64_t clocked_waits = 0;
    std::uint64_t clocked_ends = 0;
    std::uint64_t dropped = 0;
};

// OpenXR is the synchronization owner: xrWaitFrame is allowed to block and
// throttle the application.  XrFlexTimeline only records the successful frame
// loop after the runtime calls return.  It never sleeps, waits on a graphics
// queue, or changes predicted/display times.
class XrFlexTimeline {
public:
    static constexpr std::size_t kSessionSlots = 16;
    static constexpr std::size_t kFramesPerSession = 16;

    void reset() noexcept;

    [[nodiscard]] std::optional<XrFrameToken> on_wait(
        std::uintptr_t session,
        std::int64_t predicted_display_time,
        std::int64_t predicted_display_period,
        bool should_render,
        std::uint64_t wait_return_ns,
        std::uint64_t render_sequence_watermark,
        std::optional<std::uint64_t> predicted_display_host_ns = std::nullopt,
        std::uint64_t render_epoch_watermark = 0) noexcept;

    [[nodiscard]] std::optional<XrFrameToken> on_begin(
        std::uintptr_t session,
        bool discarded_previous,
        std::uint64_t begin_return_ns) noexcept;

    [[nodiscard]] std::optional<XrFrameToken> on_end(
        std::uintptr_t session,
        std::int64_t submitted_display_time,
        std::uint64_t end_return_ns,
        const std::optional<policy::CanonicalFrameToken>& latest_render,
        std::optional<std::uint64_t> submitted_display_host_ns = std::nullopt) noexcept;

    void on_destroy_session(std::uintptr_t session) noexcept;

    [[nodiscard]] std::optional<XrFrameToken> latest(std::uintptr_t session) const noexcept;
    [[nodiscard]] TimelineStats stats() const noexcept;

private:
    static constexpr std::uint64_t kBusy = ~std::uint64_t{0};

    struct FrameState {
        // sequence publishes slot identity; revision publishes coherent updates
        // after the initial wait-frame record has become visible.
        std::atomic<std::uint64_t> sequence{0};
        std::atomic<std::uint64_t> revision{0};
        std::atomic<std::uint64_t> epoch{0};
        std::atomic<std::int64_t> predicted_display_time{0};
        std::atomic<std::int64_t> predicted_display_period{0};
        std::atomic<std::int64_t> submitted_display_time{0};
        std::atomic<std::uint64_t> wait_return_ns{0};
        std::atomic<std::uint64_t> begin_return_ns{0};
        std::atomic<std::uint64_t> end_return_ns{0};
        std::atomic<std::uint64_t> predicted_display_host_ns{0};
        std::atomic<std::uint64_t> submitted_display_host_ns{0};
        std::atomic<std::int64_t> wait_to_display_ns{0};
        std::atomic<std::int64_t> end_to_display_ns{0};
        std::atomic<std::uint64_t> render_watermark{0};
        std::atomic<std::uint64_t> render_watermark_epoch{0};
        std::atomic<std::uint64_t> render_epoch{0};
        std::atomic<std::uint64_t> render_sequence{0};
        std::atomic<std::uint8_t> flags{0};
    };

    struct SessionState {
        // key owns the fixed slot; ready is the publication word. A reused
        // session key is never visible as readable state until reset completes.
        std::atomic<std::uintptr_t> key{0};
        std::atomic<bool> ready{false};
        std::atomic<std::uint64_t> epoch{1};
        std::atomic<std::uint64_t> next_sequence{0};
        std::atomic<std::uint64_t> next_begin_sequence{0};
        std::atomic<std::uint64_t> active_begin_sequence{0};
        std::atomic<std::uint64_t> latest_sequence{0};
        // Per-session statistics avoid global locked RMWs on every XR frame.
        // OpenXR externally serializes xrWaitFrame per session and xrBeginFrame/
        // xrEndFrame access to a session; different counter classes may overlap.
        std::atomic<std::uint64_t> waits{0};
        std::atomic<std::uint64_t> begins{0};
        std::atomic<std::uint64_t> ends{0};
        std::atomic<std::uint64_t> discarded{0};
        std::atomic<std::uint64_t> correlated{0};
        std::atomic<std::uint64_t> clocked_waits{0};
        std::atomic<std::uint64_t> clocked_ends{0};
        std::array<FrameState, kFramesPerSession> frames{};
    };

    enum Flag : std::uint8_t {
        ShouldRender = 1u << 0,
        Begun = 1u << 1,
        Ended = 1u << 2,
        Discarded = 1u << 3,
        PredictedClockValid = 1u << 4,
        SubmittedClockValid = 1u << 5,
    };

    std::array<SessionState, kSessionSlots> sessions_{};
    // Retired-session totals. Active sessions keep their own counters so the
    // steady frame loop pays stores rather than globally contended RMWs.
    std::atomic<std::uint64_t> waits_{0};
    std::atomic<std::uint64_t> begins_{0};
    std::atomic<std::uint64_t> ends_{0};
    std::atomic<std::uint64_t> discarded_{0};
    std::atomic<std::uint64_t> correlated_{0};
    std::atomic<std::uint64_t> clocked_waits_{0};
    std::atomic<std::uint64_t> clocked_ends_{0};
    std::atomic<std::uint64_t> dropped_{0};

    [[nodiscard]] SessionState* find_or_claim(std::uintptr_t session) noexcept;
    [[nodiscard]] SessionState* find(std::uintptr_t session) noexcept;
    [[nodiscard]] const SessionState* find(std::uintptr_t session) const noexcept;
    [[nodiscard]] static FrameState* frame_for(SessionState& state, std::uint64_t sequence) noexcept;
    [[nodiscard]] static const FrameState* frame_for(const SessionState& state, std::uint64_t sequence) noexcept;
    [[nodiscard]] static XrFrameToken token_from(
        std::uintptr_t session, const FrameState& frame) noexcept;
};

} // namespace xrflex
