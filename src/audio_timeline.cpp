#include "audio_timeline.h"

#include <algorithm>
#include <limits>

namespace audioflex {

namespace {
std::size_t slot_start(std::uintptr_t key, std::size_t capacity) noexcept {
    return ((key >> 4) ^ (key >> 13)) % capacity;
}

void atomic_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
    auto current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
}

AudioFlexTimeline::ClientState* AudioFlexTimeline::find(std::uintptr_t client) noexcept {
    if (client == 0) return nullptr;
    const auto start = slot_start(client, kClientSlots);
    for (std::size_t probe = 0; probe < kClientSlots; ++probe) {
        auto& state = clients_[(start + probe) % kClientSlots];
        const auto key = state.key.load(std::memory_order_acquire);
        if (key == client) return &state;
        // Client slots are process-lifetime entries and never erase back to 0.
        // Stop at the first never-used slot instead of scanning all 32 entries.
        if (key == 0) break;
    }
    return nullptr;
}

const AudioFlexTimeline::ClientState* AudioFlexTimeline::find(std::uintptr_t client) const noexcept {
    if (client == 0) return nullptr;
    const auto start = slot_start(client, kClientSlots);
    for (std::size_t probe = 0; probe < kClientSlots; ++probe) {
        const auto& state = clients_[(start + probe) % kClientSlots];
        const auto key = state.key.load(std::memory_order_acquire);
        if (key == client) return &state;
        if (key == 0) break;
    }
    return nullptr;
}

AudioFlexTimeline::ClientState* AudioFlexTimeline::find_or_claim(std::uintptr_t client, StreamFlow flow) noexcept {
    if (client == 0) return nullptr;
    if (auto* existing = find(client)) {
        if (flow != StreamFlow::Unknown)
            existing->flow.store(static_cast<std::uint8_t>(flow), std::memory_order_relaxed);
        return existing;
    }
    const auto start = slot_start(client, kClientSlots);
    for (std::size_t probe = 0; probe < kClientSlots; ++probe) {
        auto& state = clients_[(start + probe) % kClientSlots];
        std::uintptr_t expected = 0;
        if (!state.key.compare_exchange_strong(
                expected, kBusy, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        state.epoch.fetch_add(1, std::memory_order_relaxed);
        state.flow.store(static_cast<std::uint8_t>(flow), std::memory_order_relaxed);
        state.share_mode.store(static_cast<std::uint8_t>(ShareMode::Unknown), std::memory_order_relaxed);
        state.stream_flags.store(0, std::memory_order_relaxed);
        state.sample_rate.store(0, std::memory_order_relaxed);
        state.channels.store(0, std::memory_order_relaxed);
        state.bits_per_sample.store(0, std::memory_order_relaxed);
        state.buffer_frames.store(0, std::memory_order_relaxed);
        state.padding_frames.store(0, std::memory_order_relaxed);
        state.requested_period_frames.store(0, std::memory_order_relaxed);
        state.default_period_frames.store(0, std::memory_order_relaxed);
        state.fundamental_period_frames.store(0, std::memory_order_relaxed);
        state.min_period_frames.store(0, std::memory_order_relaxed);
        state.max_period_frames.store(0, std::memory_order_relaxed);
        state.current_period_frames.store(0, std::memory_order_relaxed);
        state.engine_sample_rate.store(0, std::memory_order_relaxed);
        state.requested_buffer_duration_100ns.store(0, std::memory_order_relaxed);
        state.requested_periodicity_100ns.store(0, std::memory_order_relaxed);
        state.stream_latency_100ns.store(0, std::memory_order_relaxed);
        state.device_default_period_100ns.store(0, std::memory_order_relaxed);
        state.device_min_period_100ns.store(0, std::memory_order_relaxed);
        state.padding_observed_host_ns.store(0, std::memory_order_relaxed);
        state.clock_frequency.store(0, std::memory_order_relaxed);
        state.clock_position.store(0, std::memory_order_relaxed);
        state.clock_qpc_host_ns.store(0, std::memory_order_relaxed);
        state.clock_observed_host_ns.store(0, std::memory_order_relaxed);
        state.next_clock_probe_host_ns.store(0, std::memory_order_relaxed);
        state.flags.store(0, std::memory_order_relaxed);
        state.key.store(client, std::memory_order_release);
        clients_count_.fetch_add(1, std::memory_order_relaxed);
        return &state;
    }
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void AudioFlexTimeline::register_client(std::uintptr_t client, StreamFlow flow) noexcept {
    (void)find_or_claim(client, flow);
}

void AudioFlexTimeline::on_initialize(
    std::uintptr_t client, StreamFlow flow, ShareMode mode, std::uint32_t flags,
    std::uint32_t sample_rate, std::uint16_t channels, std::uint16_t bits_per_sample,
    std::int64_t buffer_duration_100ns, std::int64_t periodicity_100ns,
    bool shared_stream_v3, std::uint32_t requested_period_frames) noexcept {
    auto* state = find_or_claim(client, flow);
    if (!state) return;
    // Use the spare flag bit as a lifecycle publication guard instead of
    // growing ClientState and perturbing the padding-hot cacheline stride.
    state->flags.store(Initializing, std::memory_order_release);
    state->epoch.fetch_add(1, std::memory_order_acq_rel);
    if (flow != StreamFlow::Unknown)
        state->flow.store(static_cast<std::uint8_t>(flow), std::memory_order_relaxed);
    state->share_mode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
    state->stream_flags.store(flags, std::memory_order_relaxed);
    state->sample_rate.store(sample_rate, std::memory_order_relaxed);
    state->channels.store(channels, std::memory_order_relaxed);
    state->bits_per_sample.store(bits_per_sample, std::memory_order_relaxed);
    state->requested_buffer_duration_100ns.store(buffer_duration_100ns, std::memory_order_relaxed);
    state->requested_periodicity_100ns.store(periodicity_100ns, std::memory_order_relaxed);
    state->requested_period_frames.store(requested_period_frames, std::memory_order_relaxed);
    state->buffer_frames.store(0, std::memory_order_relaxed);
    state->padding_frames.store(0, std::memory_order_relaxed);
    state->default_period_frames.store(0, std::memory_order_relaxed);
    state->fundamental_period_frames.store(0, std::memory_order_relaxed);
    state->min_period_frames.store(0, std::memory_order_relaxed);
    state->max_period_frames.store(0, std::memory_order_relaxed);
    state->current_period_frames.store(0, std::memory_order_relaxed);
    state->engine_sample_rate.store(0, std::memory_order_relaxed);
    state->stream_latency_100ns.store(0, std::memory_order_relaxed);
    state->device_default_period_100ns.store(0, std::memory_order_relaxed);
    state->device_min_period_100ns.store(0, std::memory_order_relaxed);
    state->padding_observed_host_ns.store(0, std::memory_order_relaxed);
    state->clock_frequency.store(0, std::memory_order_relaxed);
    state->clock_position.store(0, std::memory_order_relaxed);
    state->clock_qpc_host_ns.store(0, std::memory_order_relaxed);
    state->clock_observed_host_ns.store(0, std::memory_order_relaxed);
    state->next_clock_probe_host_ns.store(0, std::memory_order_relaxed);
    auto state_flags = static_cast<std::uint8_t>(Initialized);
    if (shared_stream_v3) state_flags |= SharedStreamV3;
    state->flags.store(state_flags, std::memory_order_release);
    initializations_.fetch_add(1, std::memory_order_relaxed);
}

void AudioFlexTimeline::on_buffer_size(std::uintptr_t client, std::uint32_t frames) noexcept {
    if (auto* state = find(client)) state->buffer_frames.store(frames, std::memory_order_relaxed);
}

void AudioFlexTimeline::on_padding(std::uintptr_t client, std::uint32_t frames, std::uint64_t host_ns) noexcept {
    if (auto* state = find(client)) {
        state->padding_frames.store(frames, std::memory_order_relaxed);
        state->padding_observed_host_ns.store(host_ns, std::memory_order_relaxed);
        padding_samples_.fetch_add(1, std::memory_order_relaxed);

        const auto flow = static_cast<StreamFlow>(state->flow.load(std::memory_order_relaxed));
        const auto mode = static_cast<ShareMode>(state->share_mode.load(std::memory_order_relaxed));
        const auto rate = state->sample_rate.load(std::memory_order_relaxed);
        if (flow == StreamFlow::Render && mode == ShareMode::Shared && rate != 0) {
            const auto queued_ns = frames_to_ns(frames, rate);
            queue_model_samples_.fetch_add(1, std::memory_order_relaxed);
            queue_total_ns_.fetch_add(queued_ns, std::memory_order_relaxed);
            atomic_max(queue_max_ns_, queued_ns);
        }
    }
}

void AudioFlexTimeline::on_stream_latency(std::uintptr_t client, std::int64_t latency_100ns) noexcept {
    if (auto* state = find(client)) state->stream_latency_100ns.store(latency_100ns, std::memory_order_relaxed);
}

void AudioFlexTimeline::on_device_period(std::uintptr_t client, std::int64_t default_100ns, std::int64_t min_100ns) noexcept {
    if (auto* state = find(client)) {
        state->device_default_period_100ns.store(default_100ns, std::memory_order_relaxed);
        state->device_min_period_100ns.store(min_100ns, std::memory_order_relaxed);
    }
}

void AudioFlexTimeline::on_period_range(
    std::uintptr_t client, std::uint32_t default_frames, std::uint32_t fundamental_frames,
    std::uint32_t min_frames, std::uint32_t max_frames) noexcept {
    if (auto* state = find(client)) {
        state->default_period_frames.store(default_frames, std::memory_order_relaxed);
        state->fundamental_period_frames.store(fundamental_frames, std::memory_order_relaxed);
        state->min_period_frames.store(min_frames, std::memory_order_relaxed);
        state->max_period_frames.store(max_frames, std::memory_order_relaxed);
        period_samples_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioFlexTimeline::on_current_period(std::uintptr_t client, std::uint32_t current_frames, std::uint32_t sample_rate) noexcept {
    if (auto* state = find(client)) {
        state->current_period_frames.store(current_frames, std::memory_order_relaxed);
        if (sample_rate != 0) state->engine_sample_rate.store(sample_rate, std::memory_order_relaxed);
        period_samples_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioFlexTimeline::on_start(std::uintptr_t client) noexcept {
    if (auto* state = find(client)) {
        state->flags.fetch_or(Running, std::memory_order_acq_rel);
        starts_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioFlexTimeline::on_stop(std::uintptr_t client) noexcept {
    if (auto* state = find(client)) {
        state->flags.fetch_and(static_cast<std::uint8_t>(~Running), std::memory_order_acq_rel);
        stops_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioFlexTimeline::on_active_probe(std::uintptr_t client, bool period_valid, bool clock_valid) noexcept {
    if (auto* state = find(client)) {
        auto bits = static_cast<std::uint8_t>(ActiveProbeAttempted);
        if (period_valid) bits |= PeriodProbeValid;
        if (clock_valid) bits |= ClockProbeValid;
        state->flags.fetch_or(bits, std::memory_order_acq_rel);
        active_probes_.fetch_add(1, std::memory_order_relaxed);
        if (!period_valid && !clock_valid)
            active_probe_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool AudioFlexTimeline::should_probe_clock(
    std::uintptr_t client, std::uint64_t host_ns, std::uint64_t interval_ns) noexcept {
    auto* state = find(client);
    if (!state) return false;
    auto next = state->next_clock_probe_host_ns.load(std::memory_order_relaxed);
    if (next != 0 && host_ns < next) return false;
    const auto desired = host_ns > std::numeric_limits<std::uint64_t>::max() - interval_ns
        ? std::numeric_limits<std::uint64_t>::max()
        : host_ns + interval_ns;
    return state->next_clock_probe_host_ns.compare_exchange_strong(
        next, desired, std::memory_order_acq_rel, std::memory_order_relaxed);
}

bool AudioFlexTimeline::mark_queue_model_logged(std::uintptr_t client) noexcept {
    auto* state = find(client);
    if (!state) return false;
    auto current = state->flags.load(std::memory_order_relaxed);
    while ((current & QueueModelLogged) == 0) {
        const auto desired = static_cast<std::uint8_t>(current | QueueModelLogged);
        if (state->flags.compare_exchange_weak(
                current, desired, std::memory_order_acq_rel, std::memory_order_relaxed))
            return true;
    }
    return false;
}

bool AudioFlexTimeline::queue_model_logged(std::uintptr_t client) const noexcept {
    const auto* state = find(client);
    return state && (state->flags.load(std::memory_order_relaxed) & QueueModelLogged) != 0;
}

std::uint64_t AudioFlexTimeline::clock_frequency(std::uintptr_t client) const noexcept {
    const auto* state = find(client);
    return state ? state->clock_frequency.load(std::memory_order_relaxed) : 0;
}

void AudioFlexTimeline::bind_clock(std::uintptr_t clock, std::uintptr_t client) noexcept {
    if (clock == 0 || client == 0) return;
    const auto start = slot_start(clock, kClockSlots);
    for (std::size_t probe = 0; probe < kClockSlots; ++probe) {
        auto& binding = clocks_[(start + probe) % kClockSlots];
        const auto key = binding.key.load(std::memory_order_acquire);
        if (key == clock) {
            binding.client.store(client, std::memory_order_release);
            return;
        }
        if (key != 0) continue;
        std::uintptr_t expected = 0;
        if (!binding.key.compare_exchange_strong(
                expected, kBusy, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        binding.client.store(client, std::memory_order_relaxed);
        binding.key.store(clock, std::memory_order_release);
        clock_bindings_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    dropped_.fetch_add(1, std::memory_order_relaxed);
}

std::uintptr_t AudioFlexTimeline::client_for_clock(std::uintptr_t clock) const noexcept {
    if (clock == 0) return 0;

    struct ThreadCache {
        const AudioFlexTimeline* owner = nullptr;
        std::uintptr_t clock = 0;
        const ClockBinding* binding = nullptr;
    };
    static thread_local ThreadCache cache;
    if (cache.owner == this && cache.clock == clock && cache.binding &&
        cache.binding->key.load(std::memory_order_acquire) == clock)
        return cache.binding->client.load(std::memory_order_acquire);

    const auto start = slot_start(clock, kClockSlots);
    for (std::size_t probe = 0; probe < kClockSlots; ++probe) {
        const auto& binding = clocks_[(start + probe) % kClockSlots];
        const auto key = binding.key.load(std::memory_order_acquire);
        if (key == clock) {
            cache = {this, clock, &binding};
            return binding.client.load(std::memory_order_acquire);
        }
        if (key == 0) break;
    }
    return 0;
}

void AudioFlexTimeline::on_client_clock_frequency(std::uintptr_t client, std::uint64_t frequency) noexcept {
    if (auto* state = find(client))
        state->clock_frequency.store(frequency, std::memory_order_relaxed);
}

void AudioFlexTimeline::on_client_clock_position(
    std::uintptr_t client, std::uint64_t position, std::uint64_t qpc_host_ns,
    std::uint64_t observed_host_ns) noexcept {
    if (auto* state = find(client)) {
        state->clock_position.store(position, std::memory_order_relaxed);
        state->clock_qpc_host_ns.store(qpc_host_ns, std::memory_order_relaxed);
        state->clock_observed_host_ns.store(observed_host_ns, std::memory_order_relaxed);
        clock_samples_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioFlexTimeline::on_clock_frequency(std::uintptr_t clock, std::uint64_t frequency) noexcept {
    on_client_clock_frequency(client_for_clock(clock), frequency);
}

void AudioFlexTimeline::on_clock_position(
    std::uintptr_t clock, std::uint64_t position, std::uint64_t qpc_host_ns,
    std::uint64_t observed_host_ns) noexcept {
    on_client_clock_position(client_for_clock(clock), position, qpc_host_ns, observed_host_ns);
}

AudioClientToken AudioFlexTimeline::token_from(std::uintptr_t client, const ClientState& state) const noexcept {
    AudioClientToken token{};
    const auto flags = state.flags.load(std::memory_order_acquire);
    if ((flags & Initializing) != 0) return token;
    token.client = client;
    token.epoch = state.epoch.load(std::memory_order_relaxed);
    token.flow = static_cast<StreamFlow>(state.flow.load(std::memory_order_relaxed));
    token.share_mode = static_cast<ShareMode>(state.share_mode.load(std::memory_order_relaxed));
    token.stream_flags = state.stream_flags.load(std::memory_order_relaxed);
    token.sample_rate = state.sample_rate.load(std::memory_order_relaxed);
    token.channels = state.channels.load(std::memory_order_relaxed);
    token.bits_per_sample = state.bits_per_sample.load(std::memory_order_relaxed);
    token.buffer_frames = state.buffer_frames.load(std::memory_order_relaxed);
    token.padding_frames = state.padding_frames.load(std::memory_order_relaxed);
    token.requested_period_frames = state.requested_period_frames.load(std::memory_order_relaxed);
    token.default_period_frames = state.default_period_frames.load(std::memory_order_relaxed);
    token.fundamental_period_frames = state.fundamental_period_frames.load(std::memory_order_relaxed);
    token.min_period_frames = state.min_period_frames.load(std::memory_order_relaxed);
    token.max_period_frames = state.max_period_frames.load(std::memory_order_relaxed);
    token.current_period_frames = state.current_period_frames.load(std::memory_order_relaxed);
    token.engine_sample_rate = state.engine_sample_rate.load(std::memory_order_relaxed);
    token.requested_buffer_duration_100ns = state.requested_buffer_duration_100ns.load(std::memory_order_relaxed);
    token.requested_periodicity_100ns = state.requested_periodicity_100ns.load(std::memory_order_relaxed);
    token.stream_latency_100ns = state.stream_latency_100ns.load(std::memory_order_relaxed);
    token.device_default_period_100ns = state.device_default_period_100ns.load(std::memory_order_relaxed);
    token.device_min_period_100ns = state.device_min_period_100ns.load(std::memory_order_relaxed);
    token.padding_observed_host_ns = state.padding_observed_host_ns.load(std::memory_order_relaxed);
    token.clock_frequency = state.clock_frequency.load(std::memory_order_relaxed);
    token.clock_position = state.clock_position.load(std::memory_order_relaxed);
    token.clock_qpc_host_ns = state.clock_qpc_host_ns.load(std::memory_order_relaxed);
    token.clock_observed_host_ns = state.clock_observed_host_ns.load(std::memory_order_relaxed);
    token.initialized = (flags & Initialized) != 0;
    token.running = (flags & Running) != 0;
    token.shared_stream_v3 = (flags & SharedStreamV3) != 0;
    token.active_probe_attempted = (flags & ActiveProbeAttempted) != 0;
    token.period_probe_valid = (flags & PeriodProbeValid) != 0;
    token.clock_probe_valid = (flags & ClockProbeValid) != 0;
    if (state.flags.load(std::memory_order_acquire) != flags ||
        state.key.load(std::memory_order_acquire) != client)
        return {};
    return token;
}

std::optional<AudioClientToken> AudioFlexTimeline::client(std::uintptr_t client_key) const noexcept {
    const auto* state = find(client_key);
    if (!state) return std::nullopt;
    const auto token = token_from(client_key, *state);
    return token.client != 0 ? std::optional<AudioClientToken>{token} : std::nullopt;
}

AudioQueueEstimate AudioFlexTimeline::queue_estimate(
    std::uintptr_t client_key, std::uint64_t now_ns, std::uint64_t stale_after_ns) const noexcept {
    const auto token = client(client_key);
    if (!token) return {};
    return build_queue_estimate(AudioQueueModelInput{
        .render = token->flow == StreamFlow::Render,
        .shared = token->share_mode == ShareMode::Shared,
        .initialized = token->initialized,
        .sample_rate = token->sample_rate,
        .buffer_frames = token->buffer_frames,
        .padding_frames = token->padding_frames,
        .current_period_frames = token->current_period_frames,
        .engine_sample_rate = token->engine_sample_rate,
        .default_period_frames = token->default_period_frames,
        .device_default_period_100ns = token->device_default_period_100ns,
        .stream_latency_100ns = token->stream_latency_100ns,
        .padding_observed_host_ns = token->padding_observed_host_ns,
        .clock_frequency = token->clock_frequency,
        .clock_position = token->clock_position,
        .clock_qpc_host_ns = token->clock_qpc_host_ns,
        .clock_observed_host_ns = token->clock_observed_host_ns,
    }, now_ns, stale_after_ns);
}

AudioTimelineStats AudioFlexTimeline::stats() const noexcept {
    return AudioTimelineStats{
        .clients = clients_count_.load(std::memory_order_relaxed),
        .initializations = initializations_.load(std::memory_order_relaxed),
        .starts = starts_.load(std::memory_order_relaxed),
        .stops = stops_.load(std::memory_order_relaxed),
        .padding_samples = padding_samples_.load(std::memory_order_relaxed),
        .period_samples = period_samples_.load(std::memory_order_relaxed),
        .clock_bindings = clock_bindings_.load(std::memory_order_relaxed),
        .clock_samples = clock_samples_.load(std::memory_order_relaxed),
        .active_probes = active_probes_.load(std::memory_order_relaxed),
        .active_probe_failures = active_probe_failures_.load(std::memory_order_relaxed),
        .queue_model_samples = queue_model_samples_.load(std::memory_order_relaxed),
        .queue_total_ns = queue_total_ns_.load(std::memory_order_relaxed),
        .queue_max_ns = queue_max_ns_.load(std::memory_order_relaxed),
        .dropped = dropped_.load(std::memory_order_relaxed),
    };
}

AudioFlexTimeline& timeline() noexcept {
    static AudioFlexTimeline value;
    return value;
}

} // namespace audioflex
