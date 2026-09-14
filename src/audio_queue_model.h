#pragma once

#include "audio_latency_math.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace audioflex {

enum class QueueModelConfidence : std::uint8_t {
    None = 0,
    Padding = 1,
    Buffered = 2,
    ClockCorrelated = 3,
};

struct AudioQueueModelInput {
    bool render = false;
    bool shared = false;
    bool initialized = false;
    std::uint32_t sample_rate = 0;
    std::uint32_t buffer_frames = 0;
    std::uint32_t padding_frames = 0;
    std::uint32_t current_period_frames = 0;
    std::uint32_t engine_sample_rate = 0;
    std::uint32_t default_period_frames = 0;
    std::int64_t device_default_period_100ns = 0;
    std::int64_t stream_latency_100ns = 0;
    std::uint64_t padding_observed_host_ns = 0;
    std::uint64_t clock_frequency = 0;
    std::uint64_t clock_position = 0;
    std::uint64_t clock_qpc_host_ns = 0;
    std::uint64_t clock_observed_host_ns = 0;
};

struct AudioQueueEstimate {
    bool valid = false;
    bool padding_fresh = false;
    bool period_known = false;
    bool clock_correlated = false;
    QueueModelConfidence confidence = QueueModelConfidence::None;
    std::uint32_t sample_rate = 0;
    std::uint32_t padding_frames = 0;
    std::uint32_t buffer_frames = 0;
    std::uint32_t period_frames = 0;
    std::uint16_t fill_permille = 0;
    std::uint64_t endpoint_queued_ns = 0;
    std::uint64_t buffer_capacity_ns = 0;
    std::uint64_t available_buffer_ns = 0;
    std::uint64_t engine_period_ns = 0;
    std::uint64_t stream_latency_ns = 0;
    std::uint64_t padding_age_ns = 0;
    std::uint64_t clock_sample_age_ns = 0;
    std::uint64_t clock_position_ns = 0;
};

constexpr std::uint64_t rate_units_to_ns(std::uint64_t value, std::uint64_t frequency) noexcept {
    if (frequency == 0) return 0;
    constexpr std::uint64_t kNsPerSecond = 1'000'000'000ull;
    const auto whole = value / frequency;
    const auto remainder = value % frequency;
    if (whole > std::numeric_limits<std::uint64_t>::max() / kNsPerSecond)
        return std::numeric_limits<std::uint64_t>::max();
    const auto whole_ns = whole * kNsPerSecond;
    const auto fraction_ns = static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(remainder) * kNsPerSecond) / frequency);
    if (whole_ns > std::numeric_limits<std::uint64_t>::max() - fraction_ns)
        return std::numeric_limits<std::uint64_t>::max();
    return whole_ns + fraction_ns;
}

constexpr AudioQueueEstimate build_queue_estimate(
    const AudioQueueModelInput& in, std::uint64_t now_ns,
    std::uint64_t stale_after_ns = 500'000'000ull) noexcept {
    AudioQueueEstimate out{};
    out.sample_rate = in.sample_rate;
    out.padding_frames = in.padding_frames;
    out.buffer_frames = in.buffer_frames;

    if (!in.initialized || !in.render || !in.shared || in.sample_rate == 0 || in.padding_observed_host_ns == 0)
        return out;

    out.valid = true;
    out.endpoint_queued_ns = frames_to_ns(in.padding_frames, in.sample_rate);
    out.buffer_capacity_ns = frames_to_ns(in.buffer_frames, in.sample_rate);
    const auto available_frames = in.buffer_frames > in.padding_frames
        ? static_cast<std::uint64_t>(in.buffer_frames - in.padding_frames)
        : 0ull;
    out.available_buffer_ns = frames_to_ns(available_frames, in.sample_rate);
    out.stream_latency_ns = reference_time_to_ns(in.stream_latency_100ns);

    if (now_ns >= in.padding_observed_host_ns)
        out.padding_age_ns = now_ns - in.padding_observed_host_ns;
    out.padding_fresh = out.padding_age_ns <= stale_after_ns;

    if (in.buffer_frames != 0) {
        const auto clamped = std::min(in.padding_frames, in.buffer_frames);
        out.fill_permille = static_cast<std::uint16_t>(
            (static_cast<std::uint64_t>(clamped) * 1000ull) / in.buffer_frames);
        out.confidence = QueueModelConfidence::Buffered;
    } else {
        out.confidence = QueueModelConfidence::Padding;
    }

    if (in.current_period_frames != 0) {
        out.period_frames = in.current_period_frames;
        const auto engine_rate = in.engine_sample_rate != 0 ? in.engine_sample_rate : in.sample_rate;
        out.engine_period_ns = frames_to_ns(in.current_period_frames, engine_rate);
        out.period_known = true;
    } else if (in.default_period_frames != 0) {
        out.period_frames = in.default_period_frames;
        out.engine_period_ns = frames_to_ns(in.default_period_frames, in.sample_rate);
        out.period_known = true;
    } else if (in.device_default_period_100ns > 0) {
        out.engine_period_ns = reference_time_to_ns(in.device_default_period_100ns);
        out.period_known = true;
    }

    if (in.clock_frequency != 0 && in.clock_observed_host_ns != 0) {
        out.clock_position_ns = rate_units_to_ns(in.clock_position, in.clock_frequency);
        const bool clock_not_from_future = now_ns >= in.clock_observed_host_ns;
        if (clock_not_from_future)
            out.clock_sample_age_ns = now_ns - in.clock_observed_host_ns;
        // A QPC pair is useful only while the corresponding clock sample is
        // fresh. Do not advertise clock-correlated confidence from an old (or
        // impossible future-dated) sample after padding itself has moved on.
        out.clock_correlated = clock_not_from_future &&
            out.clock_sample_age_ns <= stale_after_ns && in.clock_qpc_host_ns != 0;
        if (out.clock_correlated && out.confidence == QueueModelConfidence::Buffered)
            out.confidence = QueueModelConfidence::ClockCorrelated;
    }

    return out;
}

} // namespace audioflex
