#include "vulkan_structural_stall.h"

#include <algorithm>
#include <limits>

namespace {
constexpr std::uint64_t kMinPlausibleFrameNs = 500'000ull;
constexpr std::uint64_t kMaxPlausibleFrameNs = 100'000'000ull;
constexpr std::uint64_t kOutlierFloorNs = 4'000'000ull;

std::uint64_t saturating_double(std::uint64_t value) noexcept {
    return value > std::numeric_limits<std::uint64_t>::max() / 2ull
        ? std::numeric_limits<std::uint64_t>::max()
        : value * 2ull;
}
}

StructuralStallDecision classify_structural_stall(
    const StructuralStallConfig& config,
    const StructuralStallSample& sample) noexcept {
    StructuralStallDecision result{};
    if (config.mode == StructuralStallMode::Disabled ||
        config.pipeline_threshold_ns == 0 ||
        sample.completion_timestamp_ns == 0 ||
        sample.previous_completion_timestamp_ns == 0 ||
        sample.completion_timestamp_ns <= sample.previous_completion_timestamp_ns ||
        sample.expected_gap_ns == 0) {
        return result;
    }

    result.raw_gap_ns = sample.completion_timestamp_ns - sample.previous_completion_timestamp_ns;
    result.trusted = config.mode == StructuralStallMode::Enabled
        ? sample.baseline_samples != 0
        : sample.baseline_samples >= config.auto_warmup_samples;
    if (!result.trusted)
        return result;

    if (sample.pending_pipeline_duration_ns < config.pipeline_threshold_ns ||
        sample.pending_pipeline_end_ns == 0 ||
        sample.pending_pipeline_end_ns > sample.completion_timestamp_ns ||
        sample.completion_timestamp_ns - sample.pending_pipeline_end_ns > config.recent_window_ns) {
        return result;
    }

    const auto additive_threshold = sample.expected_gap_ns >
            std::numeric_limits<std::uint64_t>::max() - kOutlierFloorNs
        ? std::numeric_limits<std::uint64_t>::max()
        : sample.expected_gap_ns + kOutlierFloorNs;
    const auto outlier_threshold = std::max(
        saturating_double(sample.expected_gap_ns), additive_threshold);
    if (result.raw_gap_ns < outlier_threshold)
        return result;

    const auto excess = result.raw_gap_ns > sample.expected_gap_ns
        ? result.raw_gap_ns - sample.expected_gap_ns
        : 0;
    result.compensation_ns = std::min({
        sample.pending_pipeline_duration_ns,
        excess,
        config.max_compensation_ns,
    });
    result.structural = result.compensation_ns >= config.pipeline_threshold_ns;
    if (!result.structural)
        result.compensation_ns = 0;
    return result;
}

std::uint64_t update_structural_baseline_ns(
    std::uint64_t current_baseline_ns,
    std::uint64_t clean_sample_ns) noexcept {
    if (clean_sample_ns < kMinPlausibleFrameNs || clean_sample_ns > kMaxPlausibleFrameNs)
        return current_baseline_ns;
    if (current_baseline_ns == 0)
        return clean_sample_ns;

    // This baseline is a classifier reference, not a pacer. Winsorize each
    // accepted sample to [0.5x, 2x] of the current estimate before the EWMA so
    // one unrelated hitch (shader compilation, IO, scheduler preemption) cannot
    // poison the structural-stall threshold for many subsequent frames. Genuine
    // refresh/FPS changes still converge progressively instead of being frozen.
    const auto lower_bound = std::max(kMinPlausibleFrameNs, current_baseline_ns / std::uint64_t{2});
    const auto upper_bound = std::min(kMaxPlausibleFrameNs, saturating_double(current_baseline_ns));
    clean_sample_ns = std::clamp(clean_sample_ns, lower_bound, upper_bound);

    // Integer EWMA alpha=1/8.
    if (clean_sample_ns >= current_baseline_ns)
        return current_baseline_ns + (clean_sample_ns - current_baseline_ns) / 8ull;
    return current_baseline_ns - (current_baseline_ns - clean_sample_ns) / 8ull;
}

const char* structural_stall_kind_name(StructuralStallKind kind) noexcept {
    switch (kind) {
        case StructuralStallKind::GraphicsPipeline: return "graphics-pipeline";
        case StructuralStallKind::ComputePipeline: return "compute-pipeline";
    }
    return "pipeline";
}
