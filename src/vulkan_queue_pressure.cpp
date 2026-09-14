#include "vulkan_queue_pressure.h"

#include <algorithm>
#include <limits>

namespace {
constexpr std::uint64_t kNsPerUs = 1000ull;
constexpr std::uint64_t kFallbackBaseDelayNs = 500ull * kNsPerUs;
constexpr std::uint64_t kMinimumBaseDelayNs = 250ull * kNsPerUs;
constexpr std::uint8_t kMaxPressureUnits = 4;

bool auto_sample_is_trusted(const QueuePressureConfig& config, const QueuePressureSample& sample) noexcept {
    if (sample.best_tier < PresentPrecisionTier::PresentWait)
        return false;
    if (sample.precise_completions < config.auto_warmup_completions)
        return false;

    // Auto mode requires <=25% fallback completions. 3*fallback <= precise is
    // equivalent to fallback / (precise + fallback) <= 0.25 without division.
    return static_cast<std::uint64_t>(sample.fallback_completions) * 3ull <=
        static_cast<std::uint64_t>(sample.precise_completions);
}
}

QueuePressureDecision select_queue_pressure_delay(
    const QueuePressureConfig& config,
    const QueuePressureSample& sample) noexcept {
    QueuePressureDecision result{};
    if (config.mode == QueuePressureMode::Disabled || config.max_delay_us == 0)
        return result;

    result.trusted = config.mode == QueuePressureMode::Enabled
        ? sample.best_tier >= PresentPrecisionTier::PresentWait
        : auto_sample_is_trusted(config, sample);
    if (!result.trusted)
        return result;

    const auto target = static_cast<std::uint32_t>(config.target_pending_presents);
    if (sample.pending_presents <= target)
        return result;

    std::uint32_t units = sample.pending_presents - target;

    // CPU-ahead is deliberately only a secondary amplifier. It never creates
    // backpressure by itself because VF0/VF1/fallback completion timestamps are
    // not presentation-engine completion evidence.
    const std::uint64_t cpu_soft_limit = static_cast<std::uint64_t>(target) + 2ull;
    if (sample.cpu_ahead > cpu_soft_limit)
        ++units;

    units = std::min<std::uint32_t>(units, kMaxPressureUnits);
    result.pressure_units = static_cast<std::uint8_t>(units);
    result.pressured = units != 0;
    if (!result.pressured)
        return result;

    // Scale the soft delay with the observed frame period. One eighth of a
    // frame is enough to pull the producer back without turning the governor
    // into a second frame limiter. Use a small fallback before timing converges.
    std::uint64_t base_delay = sample.observed_frame_time_ns != 0
        ? std::max<std::uint64_t>(sample.observed_frame_time_ns / 8ull, kMinimumBaseDelayNs)
        : kFallbackBaseDelayNs;

    const auto max_delay_ns = static_cast<std::uint64_t>(config.max_delay_us) * kNsPerUs;
    const auto unclamped = base_delay > std::numeric_limits<std::uint64_t>::max() / units
        ? std::numeric_limits<std::uint64_t>::max()
        : base_delay * units;
    result.delay_ns = std::min(unclamped, max_delay_ns);
    return result;
}
