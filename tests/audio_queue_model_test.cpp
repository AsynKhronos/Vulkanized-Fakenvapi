#include "audio_queue_model.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using namespace audioflex;

    static_assert(rate_units_to_ns(48'000, 48'000) == 1'000'000'000ull);
    static_assert(rate_units_to_ns(24'000, 48'000) == 500'000'000ull);

    AudioQueueModelInput in{};
    in.render = true;
    in.shared = true;
    in.initialized = true;
    in.sample_rate = 48'000;
    in.buffer_frames = 1'440;
    in.padding_frames = 384;
    in.current_period_frames = 240;
    in.engine_sample_rate = 48'000;
    in.stream_latency_100ns = 100'000;
    in.padding_observed_host_ns = 1'000'000'000ull;
    in.clock_frequency = 48'000;
    in.clock_position = 96'000;
    in.clock_qpc_host_ns = 1'003'000'000ull;
    in.clock_observed_host_ns = 1'003'100'000ull;

    const auto model = build_queue_estimate(in, 1'004'000'000ull);
    assert(model.valid);
    assert(model.padding_fresh);
    assert(model.endpoint_queued_ns == 8'000'000ull);
    assert(model.buffer_capacity_ns == 30'000'000ull);
    assert(model.available_buffer_ns == 22'000'000ull);
    assert(model.engine_period_ns == 5'000'000ull);
    assert(model.stream_latency_ns == 10'000'000ull);
    assert(model.fill_permille == 266);
    assert(model.clock_correlated);
    assert(model.clock_position_ns == 2'000'000'000ull);
    assert(model.confidence == QueueModelConfidence::ClockCorrelated);

    // The current engine period can use a different engine format than the
    // client stream. Do not overwrite the stream sample rate with it.
    in.engine_sample_rate = 96'000;
    const auto mixed_rate = build_queue_estimate(in, 1'004'000'000ull);
    assert(mixed_rate.endpoint_queued_ns == 8'000'000ull);
    assert(mixed_rate.engine_period_ns == 2'500'000ull);

    // Clock confidence must decay with the sample itself. A valid QPC pair is
    // not enough once the observation is stale, and a future-dated sample must
    // never raise confidence.
    in.clock_qpc_host_ns = 1'003'000'000ull;
    in.clock_observed_host_ns = 1'003'100'000ull;
    const auto stale_clock = build_queue_estimate(in, 2'000'000'000ull, 100'000'000ull);
    assert(!stale_clock.clock_correlated);
    assert(stale_clock.confidence == QueueModelConfidence::Buffered);

    in.clock_observed_host_ns = 2'100'000'000ull;
    const auto future_clock = build_queue_estimate(in, 2'000'000'000ull, 100'000'000ull);
    assert(!future_clock.clock_correlated);
    assert(future_clock.confidence == QueueModelConfidence::Buffered);

    in.clock_qpc_host_ns = 0;
    in.clock_observed_host_ns = 1'003'100'000ull;
    const auto no_clock = build_queue_estimate(in, 2'000'000'000ull, 100'000'000ull);
    assert(no_clock.valid);
    assert(!no_clock.padding_fresh);
    assert(!no_clock.clock_correlated);
    assert(no_clock.confidence == QueueModelConfidence::Buffered);

    in.render = false;
    assert(!build_queue_estimate(in, 1'004'000'000ull).valid);
    in.render = true;
    in.shared = false;
    assert(!build_queue_estimate(in, 1'004'000'000ull).valid);

    std::cout << "AudioFlex queue model test: PASS\n";
    return 0;
}
