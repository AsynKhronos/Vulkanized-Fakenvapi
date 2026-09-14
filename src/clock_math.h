#pragma once

#include <cstdint>
#include <limits>

namespace hostclock {

// Convert an arbitrary monotonically increasing counter into nanoseconds.
// The quotient/remainder split avoids overflow for normal QPC frequencies;
// the long-double fallback is only for pathological frequencies where
// remainder * 1e9 would overflow uint64_t.
[[nodiscard]] inline std::uint64_t ticks_to_ns(
    std::uint64_t ticks, std::uint64_t frequency) noexcept {
    if (frequency == 0) return 0;
    constexpr std::uint64_t billion = 1'000'000'000ull;
    const auto whole = ticks / frequency;
    const auto remainder = ticks % frequency;
    if (whole > std::numeric_limits<std::uint64_t>::max() / billion) return 0;

    std::uint64_t fractional = 0;
    if (remainder <= std::numeric_limits<std::uint64_t>::max() / billion) {
        fractional = (remainder * billion) / frequency;
    } else {
        fractional = static_cast<std::uint64_t>(
            (static_cast<long double>(remainder) * static_cast<long double>(billion)) /
            static_cast<long double>(frequency));
    }
    return whole * billion + fractional;
}

[[nodiscard]] inline std::int64_t signed_delta_ns(
    std::uint64_t target, std::uint64_t reference) noexcept {
    constexpr auto i64_max = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    if (target >= reference) {
        const auto delta = target - reference;
        return delta > i64_max
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(delta);
    }
    const auto delta = reference - target;
    return delta > i64_max
        ? std::numeric_limits<std::int64_t>::min()
        : -static_cast<std::int64_t>(delta);
}

} // namespace hostclock
