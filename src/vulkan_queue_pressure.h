#pragma once

#include "vulkan_present_precision.h"

#include <cstdint>

enum class QueuePressureMode : std::uint8_t {
    Disabled = 0,
    Auto = 1,
    Enabled = 2,
};

struct QueuePressureConfig {
    QueuePressureMode mode = QueuePressureMode::Auto;
    std::uint8_t target_pending_presents = 1;
    std::uint32_t max_delay_us = 2000;
    std::uint8_t auto_warmup_completions = 8;
};

struct QueuePressureSample {
    std::uint32_t pending_presents = 0;
    std::uint64_t cpu_ahead = 0;
    std::uint32_t precise_completions = 0;
    std::uint32_t fallback_completions = 0;
    PresentPrecisionTier best_tier = PresentPrecisionTier::CoreWsi;
    std::uint64_t observed_frame_time_ns = 0;
};

struct QueuePressureDecision {
    bool trusted = false;
    bool pressured = false;
    std::uint8_t pressure_units = 0;
    std::uint64_t delay_ns = 0;
};

[[nodiscard]] QueuePressureDecision select_queue_pressure_delay(
    const QueuePressureConfig& config,
    const QueuePressureSample& sample) noexcept;
