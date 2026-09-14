#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace audioflex {

struct AudioPeriodRange {
    std::uint32_t sample_rate = 0;
    std::uint32_t requested_frames = 0;
    std::uint32_t default_frames = 0;
    std::uint32_t fundamental_frames = 0;
    std::uint32_t min_frames = 0;
    std::uint32_t max_frames = 0;
};

struct AudioPeriodDecision {
    bool valid = false;
    bool change = false;
    std::uint32_t requested_frames = 0;
    std::uint32_t candidate_frames = 0;
    std::uint32_t target_frames = 0;
};

constexpr std::uint32_t us_to_frames_ceil(std::uint32_t us, std::uint32_t rate) noexcept {
    if (us == 0 || rate == 0) return 0;
    const auto product = static_cast<std::uint64_t>(us) * rate;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        (product + 999'999ull) / 1'000'000ull, std::numeric_limits<std::uint32_t>::max()));
}

constexpr std::uint32_t align_period_up(std::uint32_t frames, std::uint32_t fundamental) noexcept {
    if (fundamental == 0) return frames;
    const auto rem = frames % fundamental;
    if (rem == 0) return frames;
    const auto add = fundamental - rem;
    if (frames > std::numeric_limits<std::uint32_t>::max() - add)
        return std::numeric_limits<std::uint32_t>::max();
    return frames + add;
}

constexpr AudioPeriodDecision choose_period_candidate(
    const AudioPeriodRange& in, std::uint32_t target_us,
    std::uint8_t max_reduction_percent) noexcept {
    AudioPeriodDecision out{};
    out.requested_frames = in.requested_frames;

    if (in.sample_rate == 0 || in.requested_frames == 0 || in.fundamental_frames == 0 ||
        in.min_frames == 0 || in.max_frames == 0 || in.min_frames > in.max_frames ||
        in.requested_frames < in.min_frames || in.requested_frames > in.max_frames)
        return out;

    out.valid = true;
    auto target = us_to_frames_ceil(target_us, in.sample_rate);
    target = align_period_up(target, in.fundamental_frames);
    target = std::clamp(target, in.min_frames, in.max_frames);
    out.target_frames = target;

    // Low-latency negotiation never increases the application's own period.
    if (target >= in.requested_frames) {
        out.candidate_frames = in.requested_frames;
        return out;
    }

    const auto reduction = std::clamp<std::uint32_t>(max_reduction_percent, 1u, 90u);
    const auto floor_by_step = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(in.requested_frames) * (100u - reduction) + 99u) / 100u);
    auto candidate = std::max(target, floor_by_step);
    candidate = align_period_up(candidate, in.fundamental_frames);
    candidate = std::clamp(candidate, in.min_frames, in.requested_frames);

    out.candidate_frames = candidate;
    out.change = candidate < in.requested_frames;
    return out;
}

} // namespace audioflex
