#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "audio_queue_model.h"

namespace audioflex {

enum class StreamFlow : std::uint8_t {
    Unknown = 0,
    Render = 1,
    Capture = 2,
};

enum class ShareMode : std::uint8_t {
    Unknown = 0,
    Shared = 1,
    Exclusive = 2,
};

struct AudioClientToken {
    std::uintptr_t client = 0;
    std::uint64_t epoch = 0;
    StreamFlow flow = StreamFlow::Unknown;
    ShareMode share_mode = ShareMode::Unknown;
    std::uint32_t stream_flags = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint32_t buffer_frames = 0;
    std::uint32_t padding_frames = 0;
    std::uint32_t requested_period_frames = 0;
    std::uint32_t default_period_frames = 0;
    std::uint32_t fundamental_period_frames = 0;
    std::uint32_t min_period_frames = 0;
    std::uint32_t max_period_frames = 0;
    std::uint32_t current_period_frames = 0;
    std::uint32_t engine_sample_rate = 0;
    std::int64_t requested_buffer_duration_100ns = 0;
    std::int64_t requested_periodicity_100ns = 0;
    std::int64_t stream_latency_100ns = 0;
    std::int64_t device_default_period_100ns = 0;
    std::int64_t device_min_period_100ns = 0;
    std::uint64_t padding_observed_host_ns = 0;
    std::uint64_t clock_frequency = 0;
    std::uint64_t clock_position = 0;
    std::uint64_t clock_qpc_host_ns = 0;
    std::uint64_t clock_observed_host_ns = 0;
    bool initialized = false;
    bool running = false;
    bool shared_stream_v3 = false;
    bool active_probe_attempted = false;
    bool period_probe_valid = false;
    bool clock_probe_valid = false;
};

struct AudioTimelineStats {
    std::uint64_t clients = 0;
    std::uint64_t initializations = 0;
    std::uint64_t starts = 0;
    std::uint64_t stops = 0;
    std::uint64_t padding_samples = 0;
    std::uint64_t period_samples = 0;
    std::uint64_t clock_bindings = 0;
    std::uint64_t clock_samples = 0;
    std::uint64_t active_probes = 0;
    std::uint64_t active_probe_failures = 0;
    std::uint64_t queue_model_samples = 0;
    std::uint64_t queue_total_ns = 0;
    std::uint64_t queue_max_ns = 0;
    std::uint64_t dropped = 0;
};

class AudioFlexTimeline {
public:
    static constexpr std::size_t kClientSlots = 32;
    static constexpr std::size_t kClockSlots = 32;

    AudioFlexTimeline() = default;

    void register_client(std::uintptr_t client, StreamFlow flow) noexcept;
    void on_initialize(
        std::uintptr_t client, StreamFlow flow, ShareMode mode, std::uint32_t flags,
        std::uint32_t sample_rate, std::uint16_t channels, std::uint16_t bits_per_sample,
        std::int64_t buffer_duration_100ns, std::int64_t periodicity_100ns,
        bool shared_stream_v3 = false, std::uint32_t requested_period_frames = 0) noexcept;
    void on_buffer_size(std::uintptr_t client, std::uint32_t frames) noexcept;
    void on_padding(std::uintptr_t client, std::uint32_t frames, std::uint64_t host_ns) noexcept;
    void on_stream_latency(std::uintptr_t client, std::int64_t latency_100ns) noexcept;
    void on_device_period(std::uintptr_t client, std::int64_t default_100ns, std::int64_t min_100ns) noexcept;
    void on_period_range(
        std::uintptr_t client, std::uint32_t default_frames, std::uint32_t fundamental_frames,
        std::uint32_t min_frames, std::uint32_t max_frames) noexcept;
    void on_current_period(std::uintptr_t client, std::uint32_t current_frames, std::uint32_t sample_rate) noexcept;
    void on_start(std::uintptr_t client) noexcept;
    void on_stop(std::uintptr_t client) noexcept;
    void on_active_probe(std::uintptr_t client, bool period_valid, bool clock_valid) noexcept;
    [[nodiscard]] bool should_probe_clock(
        std::uintptr_t client, std::uint64_t host_ns, std::uint64_t interval_ns) noexcept;
    [[nodiscard]] bool mark_queue_model_logged(std::uintptr_t client) noexcept;
    [[nodiscard]] bool queue_model_logged(std::uintptr_t client) const noexcept;
    [[nodiscard]] std::uint64_t clock_frequency(std::uintptr_t client) const noexcept;

    void bind_clock(std::uintptr_t clock, std::uintptr_t client) noexcept;
    void on_client_clock_frequency(std::uintptr_t client, std::uint64_t frequency) noexcept;
    void on_client_clock_position(
        std::uintptr_t client, std::uint64_t position, std::uint64_t qpc_host_ns,
        std::uint64_t observed_host_ns) noexcept;
    void on_clock_frequency(std::uintptr_t clock, std::uint64_t frequency) noexcept;
    void on_clock_position(
        std::uintptr_t clock, std::uint64_t position, std::uint64_t qpc_host_ns,
        std::uint64_t observed_host_ns) noexcept;

    [[nodiscard]] std::optional<AudioClientToken> client(std::uintptr_t client) const noexcept;
    [[nodiscard]] AudioQueueEstimate queue_estimate(
        std::uintptr_t client, std::uint64_t now_ns,
        std::uint64_t stale_after_ns = 500'000'000ull) const noexcept;
    [[nodiscard]] AudioTimelineStats stats() const noexcept;

private:
    static constexpr std::uintptr_t kBusy = ~std::uintptr_t{0};

    struct ClientState {
        std::atomic<std::uintptr_t> key{0};
        std::atomic<std::uint64_t> epoch{0};
        std::atomic<std::uint8_t> flow{0};
        std::atomic<std::uint8_t> share_mode{0};
        std::atomic<std::uint32_t> stream_flags{0};
        std::atomic<std::uint32_t> sample_rate{0};
        std::atomic<std::uint16_t> channels{0};
        std::atomic<std::uint16_t> bits_per_sample{0};
        std::atomic<std::uint32_t> buffer_frames{0};
        std::atomic<std::uint32_t> padding_frames{0};
        std::atomic<std::uint32_t> requested_period_frames{0};
        std::atomic<std::uint32_t> default_period_frames{0};
        std::atomic<std::uint32_t> fundamental_period_frames{0};
        std::atomic<std::uint32_t> min_period_frames{0};
        std::atomic<std::uint32_t> max_period_frames{0};
        std::atomic<std::uint32_t> current_period_frames{0};
        std::atomic<std::uint32_t> engine_sample_rate{0};
        std::atomic<std::int64_t> requested_buffer_duration_100ns{0};
        std::atomic<std::int64_t> requested_periodicity_100ns{0};
        std::atomic<std::int64_t> stream_latency_100ns{0};
        std::atomic<std::int64_t> device_default_period_100ns{0};
        std::atomic<std::int64_t> device_min_period_100ns{0};
        std::atomic<std::uint64_t> padding_observed_host_ns{0};
        std::atomic<std::uint64_t> clock_frequency{0};
        std::atomic<std::uint64_t> clock_position{0};
        std::atomic<std::uint64_t> clock_qpc_host_ns{0};
        std::atomic<std::uint64_t> clock_observed_host_ns{0};
        std::atomic<std::uint64_t> next_clock_probe_host_ns{0};
        std::atomic<std::uint8_t> flags{0};
    };

    struct ClockBinding {
        std::atomic<std::uintptr_t> key{0};
        std::atomic<std::uintptr_t> client{0};
    };

    enum StateFlags : std::uint8_t {
        Initialized = 1u << 0,
        Running = 1u << 1,
        SharedStreamV3 = 1u << 2,
        ActiveProbeAttempted = 1u << 3,
        PeriodProbeValid = 1u << 4,
        ClockProbeValid = 1u << 5,
        QueueModelLogged = 1u << 6,
        Initializing = 1u << 7,
    };

    ClientState* find(std::uintptr_t client) noexcept;
    const ClientState* find(std::uintptr_t client) const noexcept;
    ClientState* find_or_claim(std::uintptr_t client, StreamFlow flow) noexcept;
    std::uintptr_t client_for_clock(std::uintptr_t clock) const noexcept;
    AudioClientToken token_from(std::uintptr_t client, const ClientState& state) const noexcept;

    std::array<ClientState, kClientSlots> clients_{};
    std::array<ClockBinding, kClockSlots> clocks_{};
    std::atomic<std::uint64_t> clients_count_{0};
    std::atomic<std::uint64_t> initializations_{0};
    std::atomic<std::uint64_t> starts_{0};
    std::atomic<std::uint64_t> stops_{0};
    std::atomic<std::uint64_t> padding_samples_{0};
    std::atomic<std::uint64_t> period_samples_{0};
    std::atomic<std::uint64_t> clock_bindings_{0};
    std::atomic<std::uint64_t> clock_samples_{0};
    std::atomic<std::uint64_t> active_probes_{0};
    std::atomic<std::uint64_t> active_probe_failures_{0};
    std::atomic<std::uint64_t> queue_model_samples_{0};
    std::atomic<std::uint64_t> queue_total_ns_{0};
    std::atomic<std::uint64_t> queue_max_ns_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

AudioFlexTimeline& timeline() noexcept;

} // namespace audioflex
