#include "audio_latency_math.h"
#include "audio_timeline.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using namespace audioflex;

    static_assert(frames_to_ns(480, 48'000) == 10'000'000ull);
    static_assert(frames_to_ns(48, 48'000) == 1'000'000ull);
    static_assert(reference_time_to_ns(100'000) == 10'000'000ull);
    static_assert(hundred_ns_to_ns(123) == 12'300ull);

    AudioFlexTimeline timeline;
    constexpr std::uintptr_t client = 0x1000;
    constexpr std::uintptr_t clock = 0x2000;

    timeline.register_client(client, StreamFlow::Render);
    timeline.on_initialize(
        client, StreamFlow::Render, ShareMode::Shared, 0x40000,
        48'000, 2, 32, 100'000, 0);
    timeline.on_buffer_size(client, 480);
    timeline.on_stream_latency(client, 75'000);
    timeline.on_device_period(client, 100'000, 30'000);
    timeline.on_period_range(client, 480, 48, 96, 480);
    timeline.on_current_period(client, 240, 48'000);
    timeline.on_padding(client, 192, 1'000'000'000ull);
    timeline.on_start(client);

    timeline.bind_clock(clock, client);
    timeline.on_clock_frequency(clock, 48'000);
    timeline.on_clock_position(clock, 9'600, 1'003'000'000ull, 1'003'100'000ull);

    auto token = timeline.client(client);
    assert(token);
    assert(token->initialized);
    assert(token->running);
    assert(token->flow == StreamFlow::Render);
    assert(token->share_mode == ShareMode::Shared);
    assert(token->sample_rate == 48'000);
    assert(token->buffer_frames == 480);
    assert(token->padding_frames == 192);
    assert(frames_to_ns(token->buffer_frames, token->sample_rate) == 10'000'000ull);
    assert(frames_to_ns(token->padding_frames, token->sample_rate) == 4'000'000ull);
    assert(token->default_period_frames == 480);
    assert(token->fundamental_period_frames == 48);
    assert(token->min_period_frames == 96);
    assert(token->max_period_frames == 480);
    assert(token->current_period_frames == 240);
    assert(token->engine_sample_rate == 48'000);
    assert(token->clock_frequency == 48'000);
    assert(token->clock_position == 9'600);
    assert(token->clock_qpc_host_ns == 1'003'000'000ull);
    assert(timeline.clock_frequency(client) == 48'000);
    assert(!timeline.queue_model_logged(client));

    const auto estimate = timeline.queue_estimate(client, 1'003'200'000ull);
    assert(estimate.valid);
    assert(estimate.endpoint_queued_ns == 4'000'000ull);
    assert(estimate.buffer_capacity_ns == 10'000'000ull);
    assert(estimate.engine_period_ns == 5'000'000ull);
    assert(estimate.clock_correlated);

    timeline.on_active_probe(client, true, true);
    token = timeline.client(client);
    assert(token && token->active_probe_attempted);
    assert(token->period_probe_valid);
    assert(token->clock_probe_valid);
    assert(timeline.should_probe_clock(client, 2'000'000'000ull, 250'000'000ull));
    assert(!timeline.should_probe_clock(client, 2'100'000'000ull, 250'000'000ull));
    assert(timeline.should_probe_clock(client, 2'250'000'000ull, 250'000'000ull));
    assert(timeline.mark_queue_model_logged(client));
    assert(timeline.queue_model_logged(client));
    assert(!timeline.mark_queue_model_logged(client));

    timeline.on_stop(client);
    token = timeline.client(client);
    assert(token && !token->running);

    // Reinitialization is an epoch boundary and must clear stale buffer/padding.
    const auto old_epoch = token->epoch;
    timeline.on_initialize(
        client, StreamFlow::Render, ShareMode::Shared, 0,
        44'100, 2, 24, 0, 0, true, 128);
    token = timeline.client(client);
    assert(token);
    assert(token->epoch > old_epoch);
    assert(token->shared_stream_v3);
    assert(token->requested_period_frames == 128);
    assert(token->buffer_frames == 0);
    assert(token->padding_frames == 0);
    assert(timeline.clock_frequency(client) == 0);
    assert(!timeline.queue_model_logged(client));

    const auto stats = timeline.stats();
    assert(stats.clients == 1);
    assert(stats.initializations == 2);
    assert(stats.starts == 1);
    assert(stats.stops == 1);
    assert(stats.padding_samples == 1);
    assert(stats.period_samples == 2);
    assert(stats.clock_bindings == 1);
    assert(stats.clock_samples == 1);
    assert(stats.active_probes == 1);
    assert(stats.active_probe_failures == 0);
    assert(stats.queue_model_samples == 1);
    assert(stats.queue_total_ns == 4'000'000ull);
    assert(stats.queue_max_ns == 4'000'000ull);
    assert(stats.dropped == 0);

    std::cout << "AudioFlex timeline test: PASS\n";
    return 0;
}
