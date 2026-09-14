#pragma once

#include <cstdint>
#include <limits>

namespace audioflex {

constexpr std::uint64_t frames_to_ns(std::uint64_t frames, std::uint32_t sample_rate) noexcept {
    if (sample_rate == 0) return 0;
    constexpr std::uint64_t kNsPerSecond = 1'000'000'000ull;
    const auto whole = frames / sample_rate;
    const auto remainder = frames % sample_rate;
    if (whole > std::numeric_limits<std::uint64_t>::max() / kNsPerSecond)
        return std::numeric_limits<std::uint64_t>::max();
    const auto whole_ns = whole * kNsPerSecond;
    const auto fraction_ns = (remainder * kNsPerSecond) / sample_rate;
    if (whole_ns > std::numeric_limits<std::uint64_t>::max() - fraction_ns)
        return std::numeric_limits<std::uint64_t>::max();
    return whole_ns + fraction_ns;
}

constexpr std::uint64_t hundred_ns_to_ns(std::uint64_t value) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() / 100ull)
        return std::numeric_limits<std::uint64_t>::max();
    return value * 100ull;
}

constexpr std::uint64_t reference_time_to_ns(std::int64_t value) noexcept {
    if (value <= 0) return 0;
    return hundred_ns_to_ns(static_cast<std::uint64_t>(value));
}

} // namespace audioflex
