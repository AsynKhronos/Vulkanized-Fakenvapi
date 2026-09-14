#pragma once

#include <cstdint>

enum class StructuralStallMode : std::uint8_t {
    Disabled = 0,
    Auto = 1,
    Enabled = 2,
};

enum class StructuralStallKind : std::uint8_t {
    GraphicsPipeline = 0,
    ComputePipeline = 1,
};

struct StructuralStallConfig {
    StructuralStallMode mode = StructuralStallMode::Auto;
    std::uint64_t pipeline_threshold_ns = 2'000'000ull;
    std::uint64_t recent_window_ns = 100'000'000ull;
    std::uint64_t max_compensation_ns = 50'000'000ull;
    std::uint8_t auto_warmup_samples = 8;
};

struct StructuralStallSample {
    std::uint64_t completion_timestamp_ns = 0;
    std::uint64_t previous_completion_timestamp_ns = 0;
    std::uint64_t expected_gap_ns = 0;
    std::uint32_t baseline_samples = 0;
    std::uint64_t pending_pipeline_duration_ns = 0;
    std::uint64_t pending_pipeline_end_ns = 0;
};

struct StructuralStallDecision {
    bool trusted = false;
    bool structural = false;
    std::uint64_t raw_gap_ns = 0;
    std::uint64_t compensation_ns = 0;
};

[[nodiscard]] StructuralStallDecision classify_structural_stall(
    const StructuralStallConfig& config,
    const StructuralStallSample& sample) noexcept;

[[nodiscard]] std::uint64_t update_structural_baseline_ns(
    std::uint64_t current_baseline_ns,
    std::uint64_t clean_sample_ns) noexcept;

[[nodiscard]] const char* structural_stall_kind_name(StructuralStallKind kind) noexcept;
