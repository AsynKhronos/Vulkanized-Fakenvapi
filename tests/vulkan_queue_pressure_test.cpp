#include "vulkan_queue_pressure.h"

#include <cassert>

int main() {
    QueuePressureConfig config{};
    QueuePressureSample sample{};
    sample.best_tier = PresentPrecisionTier::PresentWait;
    sample.pending_presents = 2;
    sample.precise_completions = 8;
    sample.observed_frame_time_ns = 8'000'000;

    auto decision = select_queue_pressure_delay(config, sample);
    assert(decision.trusted);
    assert(decision.pressured);
    assert(decision.pressure_units == 1);
    assert(decision.delay_ns == 1'000'000);

    // Auto mode does not act before precision completion has warmed up.
    sample.precise_completions = 7;
    decision = select_queue_pressure_delay(config, sample);
    assert(!decision.trusted);
    assert(decision.delay_ns == 0);

    // Too many CPU-present fallbacks make Auto fail open.
    sample.precise_completions = 8;
    sample.fallback_completions = 3;
    decision = select_queue_pressure_delay(config, sample);
    assert(!decision.trusted);

    // Forced Enabled bypasses warmup but still requires a synchronous
    // presentation-completion tier (VF2+).
    config.mode = QueuePressureMode::Enabled;
    sample.precise_completions = 0;
    sample.fallback_completions = 100;
    decision = select_queue_pressure_delay(config, sample);
    assert(decision.trusted);
    assert(decision.delay_ns == 1'000'000);

    sample.best_tier = PresentPrecisionTier::PresentId;
    decision = select_queue_pressure_delay(config, sample);
    assert(!decision.trusted);

    // Meeting the target never adds a delay.
    sample.best_tier = PresentPrecisionTier::PresentWait2;
    sample.pending_presents = 1;
    decision = select_queue_pressure_delay(config, sample);
    assert(decision.trusted);
    assert(!decision.pressured);
    assert(decision.delay_ns == 0);

    // CPU-ahead can amplify real present pressure, but cannot create it alone.
    sample.pending_presents = 2;
    sample.cpu_ahead = 5;
    decision = select_queue_pressure_delay(config, sample);
    assert(decision.pressure_units == 2);
    assert(decision.delay_ns == 2'000'000);

    sample.pending_presents = 1;
    decision = select_queue_pressure_delay(config, sample);
    assert(decision.delay_ns == 0);

    // max_delay_us is a hard soft-governor cap under our own eepy wait.
    sample.pending_presents = 8;
    sample.cpu_ahead = 8;
    sample.observed_frame_time_ns = 16'000'000;
    config.max_delay_us = 1500;
    decision = select_queue_pressure_delay(config, sample);
    assert(decision.pressure_units == 4);
    assert(decision.delay_ns == 1'500'000);

    config.mode = QueuePressureMode::Disabled;
    decision = select_queue_pressure_delay(config, sample);
    assert(!decision.trusted);
    assert(decision.delay_ns == 0);
    return 0;
}
