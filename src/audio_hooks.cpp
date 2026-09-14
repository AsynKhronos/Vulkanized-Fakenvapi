#include "audio_hooks.h"

#include "audio_abi.h"
#include "audio_latency_math.h"
#include "audio_period_policy.h"
#include "audio_timeline.h"
#include "config.h"
#include "log.h"
#include "util.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <detours.h>

namespace {

std::mutex g_hook_mutex;
bool g_initialized = false;
thread_local bool g_probe_call = false;

using namespace audioabi;

CoCreateInstanceFn g_co_create_instance = nullptr;
CoTaskMemFreeFn g_co_task_mem_free = nullptr;
EnumAudioEndpointsFn g_enum_audio_endpoints = nullptr;
GetDefaultAudioEndpointFn g_get_default_audio_endpoint = nullptr;
GetDeviceFn g_get_device = nullptr;
CollectionItemFn g_collection_item = nullptr;
DeviceActivateFn g_device_activate = nullptr;
AudioInitializeFn g_audio_initialize = nullptr;
AudioGetBufferSizeFn g_audio_get_buffer_size = nullptr;
AudioGetStreamLatencyFn g_audio_get_stream_latency = nullptr;
AudioGetCurrentPaddingFn g_audio_get_current_padding = nullptr;
AudioGetDevicePeriodFn g_audio_get_device_period = nullptr;
AudioStartStopFn g_audio_start = nullptr;
AudioStartStopFn g_audio_stop = nullptr;
AudioGetServiceFn g_audio_get_service = nullptr;
AudioGetSharedModeEnginePeriodFn g_audio_get_shared_period = nullptr;
AudioGetCurrentSharedModeEnginePeriodFn g_audio_get_current_period = nullptr;
AudioInitializeSharedAudioStreamFn g_audio_initialize_shared = nullptr;
AudioClockGetFrequencyFn g_clock_get_frequency = nullptr;
AudioClockGetPositionFn g_clock_get_position = nullptr;

bool g_co_attached = false;
bool g_enum_attached = false;
bool g_default_attached = false;
bool g_get_device_attached = false;
bool g_collection_item_attached = false;
bool g_activate_attached = false;
bool g_initialize_attached = false;
bool g_buffer_size_attached = false;
bool g_stream_latency_attached = false;
bool g_padding_attached = false;
bool g_device_period_attached = false;
bool g_start_attached = false;
bool g_stop_attached = false;
bool g_service_attached = false;
bool g_shared_period_attached = false;
bool g_current_period_attached = false;
bool g_initialize_shared_attached = false;
bool g_clock_frequency_attached = false;
bool g_clock_position_attached = false;

constexpr std::size_t kBindingSlots = 32;
constexpr std::uintptr_t kBusy = ~std::uintptr_t{0};

struct FlowBinding {
    std::atomic<std::uintptr_t> key{0};
    std::atomic<std::uint8_t> flow{0};
};

struct AliasBinding {
    std::atomic<std::uintptr_t> key{0};
    std::atomic<std::uintptr_t> canonical{0};
};

std::array<FlowBinding, kBindingSlots> g_endpoint_flows{};
std::array<FlowBinding, kBindingSlots> g_collection_flows{};
std::array<AliasBinding, kBindingSlots * 2> g_client_aliases{};

bool audio_enabled() noexcept {
    const auto& cfg = Config::get().snapshot().audio;
    return cfg.enabled && cfg.wasapi_observer;
}

bool audio_clock_enabled() noexcept {
    const auto& cfg = Config::get().snapshot().audio;
    return cfg.enabled && cfg.wasapi_observer && cfg.clock;
}

bool audio_active_probe_enabled() noexcept {
    const auto& cfg = Config::get().snapshot().audio;
    return cfg.enabled && cfg.wasapi_observer && cfg.active_probe;
}

policy::AutoBool audio_adaptive_period_mode() noexcept {
    return Config::get().snapshot().audio.adaptive_period;
}

struct ProbeCallScope {
    bool previous = false;
    ProbeCallScope() noexcept : previous(g_probe_call) { g_probe_call = true; }
    ~ProbeCallScope() { g_probe_call = previous; }
};

template <typename T>
T com_method(void* object, std::size_t index) noexcept {
    auto** table = vtable(object);
    return table ? reinterpret_cast<T>(table[index]) : nullptr;
}

std::size_t binding_start(std::uintptr_t key, std::size_t count) noexcept {
    return ((key >> 4) ^ (key >> 13)) % count;
}

audioflex::StreamFlow to_flow(DataFlow value) noexcept {
    switch (value) {
        case DataFlow::Render: return audioflex::StreamFlow::Render;
        case DataFlow::Capture: return audioflex::StreamFlow::Capture;
        default: return audioflex::StreamFlow::Unknown;
    }
}

const char* flow_name(audioflex::StreamFlow value) noexcept {
    switch (value) {
        case audioflex::StreamFlow::Render: return "render";
        case audioflex::StreamFlow::Capture: return "capture";
        default: return "unknown";
    }
}

const char* mode_name(audioflex::ShareMode value) noexcept {
    switch (value) {
        case audioflex::ShareMode::Shared: return "shared";
        case audioflex::ShareMode::Exclusive: return "exclusive";
        default: return "unknown";
    }
}

audioflex::ShareMode to_share_mode(ShareMode value) noexcept {
    return value == ShareMode::Exclusive
        ? audioflex::ShareMode::Exclusive
        : audioflex::ShareMode::Shared;
}

void publish_flow(std::array<FlowBinding, kBindingSlots>& table,
                  std::uintptr_t key, audioflex::StreamFlow flow) noexcept {
    if (key == 0) return;
    const auto start = binding_start(key, table.size());
    for (std::size_t probe = 0; probe < table.size(); ++probe) {
        auto& binding = table[(start + probe) % table.size()];
        const auto current = binding.key.load(std::memory_order_acquire);
        if (current == key) {
            if (flow != audioflex::StreamFlow::Unknown)
                binding.flow.store(static_cast<std::uint8_t>(flow), std::memory_order_release);
            return;
        }
        if (current != 0) continue;
        std::uintptr_t expected = 0;
        if (!binding.key.compare_exchange_strong(
                expected, kBusy, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        binding.flow.store(static_cast<std::uint8_t>(flow), std::memory_order_relaxed);
        binding.key.store(key, std::memory_order_release);
        return;
    }
}

audioflex::StreamFlow lookup_flow(
    const std::array<FlowBinding, kBindingSlots>& table, std::uintptr_t key) noexcept {
    if (key == 0) return audioflex::StreamFlow::Unknown;
    const auto start = binding_start(key, table.size());
    for (std::size_t probe = 0; probe < table.size(); ++probe) {
        const auto& binding = table[(start + probe) % table.size()];
        const auto published = binding.key.load(std::memory_order_acquire);
        if (published == key)
            return static_cast<audioflex::StreamFlow>(binding.flow.load(std::memory_order_acquire));
        if (published == 0) break;
    }
    return audioflex::StreamFlow::Unknown;
}

void publish_alias(std::uintptr_t alias, std::uintptr_t canonical) noexcept {
    if (alias == 0 || canonical == 0) return;
    const auto start = binding_start(alias, g_client_aliases.size());
    for (std::size_t probe = 0; probe < g_client_aliases.size(); ++probe) {
        auto& binding = g_client_aliases[(start + probe) % g_client_aliases.size()];
        const auto current = binding.key.load(std::memory_order_acquire);
        if (current == alias) {
            binding.canonical.store(canonical, std::memory_order_release);
            return;
        }
        if (current != 0) continue;
        std::uintptr_t expected = 0;
        if (!binding.key.compare_exchange_strong(
                expected, kBusy, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        binding.canonical.store(canonical, std::memory_order_relaxed);
        binding.key.store(alias, std::memory_order_release);
        return;
    }
}

std::uintptr_t canonical_client(void* object) noexcept {
    const auto key = reinterpret_cast<std::uintptr_t>(object);
    if (key == 0) return 0;

    const auto start = binding_start(key, g_client_aliases.size());
    const auto& first = g_client_aliases[start];
    const auto first_key = first.key.load(std::memory_order_acquire);
    if (first_key == key) {
        const auto canonical = first.canonical.load(std::memory_order_acquire);
        return canonical ? canonical : key;
    }
    if (first_key == 0) return key;

    // Only colliding aliases pay TLS-cache bookkeeping. This preserves the
    // single acquire-load common case while avoiding repeated 64-slot walks for
    // interfaces that do collide in the fixed registry.
    static thread_local std::uintptr_t cached_key = 0;
    static thread_local std::size_t cached_slot = g_client_aliases.size();
    if (cached_key == key && cached_slot < g_client_aliases.size()) {
        const auto& binding = g_client_aliases[cached_slot];
        if (binding.key.load(std::memory_order_acquire) == key) {
            const auto canonical = binding.canonical.load(std::memory_order_acquire);
            return canonical ? canonical : key;
        }
    }

    for (std::size_t probe = 1; probe < g_client_aliases.size(); ++probe) {
        const auto index = (start + probe) % g_client_aliases.size();
        const auto& binding = g_client_aliases[index];
        const auto published = binding.key.load(std::memory_order_acquire);
        if (published == key) {
            cached_key = key;
            cached_slot = index;
            const auto canonical = binding.canonical.load(std::memory_order_acquire);
            return canonical ? canonical : key;
        }
        if (published == 0) break;
    }
    return key;
}

// Forward declarations for dynamically attached COM entry points.
HRESULT WINAPI hkCoCreateInstance(REFCLSID, void*, DWORD, REFIID, void**);
HRESULT WINAPI hkEnumAudioEndpoints(void*, DataFlow, DWORD, void**);
HRESULT WINAPI hkGetDefaultAudioEndpoint(void*, DataFlow, int, void**);
HRESULT WINAPI hkGetDevice(void*, LPCWSTR, void**);
HRESULT WINAPI hkCollectionItem(void*, UINT, void**);
HRESULT WINAPI hkDeviceActivate(void*, REFIID, DWORD, void*, void**);
HRESULT WINAPI hkAudioInitialize(void*, ShareMode, DWORD, ReferenceTime, ReferenceTime, const WaveFormatEx*, const GUID*);
HRESULT WINAPI hkAudioGetBufferSize(void*, UINT32*);
HRESULT WINAPI hkAudioGetStreamLatency(void*, ReferenceTime*);
HRESULT WINAPI hkAudioGetCurrentPadding(void*, UINT32*);
HRESULT WINAPI hkAudioGetDevicePeriod(void*, ReferenceTime*, ReferenceTime*);
HRESULT WINAPI hkAudioStart(void*);
HRESULT WINAPI hkAudioStop(void*);
HRESULT WINAPI hkAudioGetService(void*, REFIID, void**);
HRESULT WINAPI hkAudioGetSharedModeEnginePeriod(void*, const WaveFormatEx*, UINT32*, UINT32*, UINT32*, UINT32*);
HRESULT WINAPI hkAudioGetCurrentSharedModeEnginePeriod(void*, WaveFormatEx**, UINT32*);
HRESULT WINAPI hkAudioInitializeSharedAudioStream(void*, DWORD, UINT32, const WaveFormatEx*, const GUID*);
HRESULT WINAPI hkAudioClockGetFrequency(void*, UINT64*);
HRESULT WINAPI hkAudioClockGetPosition(void*, UINT64*, UINT64*);

template <typename T>
bool attach_once(T& slot, bool& attached, void* target, void* hook, const char* label) {
    if (attached || !target) return attached;
    slot = reinterpret_cast<T>(target);
    if (DetourTransactionBegin() != NO_ERROR) return false;
    DetourUpdateThread(GetCurrentThread());
    const auto attach_status = DetourAttach(reinterpret_cast<PVOID*>(&slot), hook);
    const auto commit_status = attach_status == NO_ERROR ? DetourTransactionCommit() : ERROR_INVALID_FUNCTION;
    if (attach_status != NO_ERROR) DetourTransactionAbort();
    if (attach_status == NO_ERROR && commit_status == NO_ERROR) {
        attached = true;
        spdlog::debug("AudioFlex hooked {}", label);
        return true;
    }
    spdlog::warn("AudioFlex failed to hook {}: attach={}, commit={}", label, attach_status, commit_status);
    return false;
}

template <typename T>
void detach_if(T& slot, bool& attached, void* hook) noexcept {
    if (!attached || !slot) return;
    if (DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    if (DetourDetach(reinterpret_cast<PVOID*>(&slot), hook) == NO_ERROR) {
        if (DetourTransactionCommit() == NO_ERROR) {
            attached = false;
            return;
        }
    } else {
        DetourTransactionAbort();
    }
}

void observe_collection(void* collection, audioflex::StreamFlow flow) {
    if (!collection) return;
    publish_flow(g_collection_flows, reinterpret_cast<std::uintptr_t>(collection), flow);
    auto** table = vtable(collection);
    if (!table) return;
    std::scoped_lock lock(g_hook_mutex);
    attach_once(g_collection_item, g_collection_item_attached, table[4],
                reinterpret_cast<void*>(hkCollectionItem), "IMMDeviceCollection::Item");
}

void observe_endpoint(void* endpoint, audioflex::StreamFlow flow) {
    if (!endpoint) return;
    publish_flow(g_endpoint_flows, reinterpret_cast<std::uintptr_t>(endpoint), flow);
    auto** table = vtable(endpoint);
    if (!table) return;
    std::scoped_lock lock(g_hook_mutex);
    attach_once(g_device_activate, g_activate_attached, table[3],
                reinterpret_cast<void*>(hkDeviceActivate), "IMMDevice::Activate");
}

void observe_enumerator(void* enumerator) {
    if (!enumerator) return;
    auto** table = vtable(enumerator);
    if (!table) return;
    std::scoped_lock lock(g_hook_mutex);
    attach_once(g_enum_audio_endpoints, g_enum_attached, table[3],
                reinterpret_cast<void*>(hkEnumAudioEndpoints), "IMMDeviceEnumerator::EnumAudioEndpoints");
    attach_once(g_get_default_audio_endpoint, g_default_attached, table[4],
                reinterpret_cast<void*>(hkGetDefaultAudioEndpoint), "IMMDeviceEnumerator::GetDefaultAudioEndpoint");
    attach_once(g_get_device, g_get_device_attached, table[5],
                reinterpret_cast<void*>(hkGetDevice), "IMMDeviceEnumerator::GetDevice");
}

void observe_clock(void* clock, std::uintptr_t client) {
    if (!clock || client == 0) return;
    audioflex::timeline().bind_clock(reinterpret_cast<std::uintptr_t>(clock), client);
    auto** table = vtable(clock);
    if (!table) return;
    std::scoped_lock lock(g_hook_mutex);
    attach_once(g_clock_get_frequency, g_clock_frequency_attached, table[3],
                reinterpret_cast<void*>(hkAudioClockGetFrequency), "IAudioClock::GetFrequency");
    attach_once(g_clock_get_position, g_clock_position_attached, table[4],
                reinterpret_cast<void*>(hkAudioClockGetPosition), "IAudioClock::GetPosition");
}

void hook_audio_client3(void* client3) {
    if (!client3) return;
    auto** table = vtable(client3);
    if (!table) return;
    std::scoped_lock lock(g_hook_mutex);
    attach_once(g_audio_get_shared_period, g_shared_period_attached, table[18],
                reinterpret_cast<void*>(hkAudioGetSharedModeEnginePeriod), "IAudioClient3::GetSharedModeEnginePeriod");
    attach_once(g_audio_get_current_period, g_current_period_attached, table[19],
                reinterpret_cast<void*>(hkAudioGetCurrentSharedModeEnginePeriod), "IAudioClient3::GetCurrentSharedModeEnginePeriod");
    attach_once(g_audio_initialize_shared, g_initialize_shared_attached, table[20],
                reinterpret_cast<void*>(hkAudioInitializeSharedAudioStream), "IAudioClient3::InitializeSharedAudioStream");
}

void observe_audio_client(void* client, audioflex::StreamFlow flow) {
    if (!client) return;
    const auto canonical = reinterpret_cast<std::uintptr_t>(client);
    publish_alias(canonical, canonical);
    audioflex::timeline().register_client(canonical, flow);

    auto** table = vtable(client);
    if (table) {
        std::scoped_lock lock(g_hook_mutex);
        attach_once(g_audio_initialize, g_initialize_attached, table[3],
                    reinterpret_cast<void*>(hkAudioInitialize), "IAudioClient::Initialize");
        attach_once(g_audio_get_buffer_size, g_buffer_size_attached, table[4],
                    reinterpret_cast<void*>(hkAudioGetBufferSize), "IAudioClient::GetBufferSize");
        attach_once(g_audio_get_stream_latency, g_stream_latency_attached, table[5],
                    reinterpret_cast<void*>(hkAudioGetStreamLatency), "IAudioClient::GetStreamLatency");
        attach_once(g_audio_get_current_padding, g_padding_attached, table[6],
                    reinterpret_cast<void*>(hkAudioGetCurrentPadding), "IAudioClient::GetCurrentPadding");
        attach_once(g_audio_get_device_period, g_device_period_attached, table[9],
                    reinterpret_cast<void*>(hkAudioGetDevicePeriod), "IAudioClient::GetDevicePeriod");
        attach_once(g_audio_start, g_start_attached, table[10],
                    reinterpret_cast<void*>(hkAudioStart), "IAudioClient::Start");
        attach_once(g_audio_stop, g_stop_attached, table[11],
                    reinterpret_cast<void*>(hkAudioStop), "IAudioClient::Stop");
        attach_once(g_audio_get_service, g_service_attached, table[14],
                    reinterpret_cast<void*>(hkAudioGetService), "IAudioClient::GetService");
    }

    void* client3 = nullptr;
    if (SUCCEEDED(query_interface(client, IID_IAudioClient3_, &client3)) && client3) {
        publish_alias(reinterpret_cast<std::uintptr_t>(client3), canonical);
        hook_audio_client3(client3);
        release(client3);
    }
}


void probe_clock_once(void* client_iface, std::uintptr_t client, bool& clock_valid) {
    if (!client_iface || client == 0) return;
    const auto get_service = com_method<AudioGetServiceFn>(client_iface, 14);
    if (!get_service) return;

    void* clock = nullptr;
    {
        ProbeCallScope scope;
        if (FAILED(get_service(client_iface, IID_IAudioClock_, &clock)) || !clock) return;
    }

    UINT64 frequency = audioflex::timeline().clock_frequency(client);
    UINT64 position = 0;
    UINT64 qpc_100ns = 0;
    bool frequency_ok = frequency != 0;
    bool position_ok = false;

    // IAudioClock::GetFrequency is invariant for the initialized stream. Once
    // observed, periodic active probes only need GetPosition; reinitialization
    // clears the cached frequency in AudioFlexTimeline.
    if (!frequency_ok) {
        if (const auto get_frequency = com_method<AudioClockGetFrequencyFn>(clock, 3)) {
            ProbeCallScope scope;
            if (SUCCEEDED(get_frequency(clock, &frequency)) && frequency != 0) {
                audioflex::timeline().on_client_clock_frequency(client, frequency);
                frequency_ok = true;
            }
        }
    }
    if (const auto get_position = com_method<AudioClockGetPositionFn>(clock, 4)) {
        ProbeCallScope scope;
        if (SUCCEEDED(get_position(clock, &position, &qpc_100ns))) {
            audioflex::timeline().on_client_clock_position(
                client, position, audioflex::hundred_ns_to_ns(qpc_100ns), get_timestamp());
            position_ok = true;
        }
    }

    clock_valid = clock_valid || (frequency_ok && position_ok);
    release(clock);
}

void probe_audio_client_after_initialize(
    void* client_iface, std::uintptr_t client, audioflex::ShareMode mode,
    const WaveFormatEx* format) {
    if (!audio_active_probe_enabled() || !client_iface || client == 0) return;

    bool period_valid = false;
    bool clock_valid = false;

    if (const auto get_buffer = com_method<AudioGetBufferSizeFn>(client_iface, 4)) {
        UINT32 frames = 0;
        ProbeCallScope scope;
        if (SUCCEEDED(get_buffer(client_iface, &frames)))
            audioflex::timeline().on_buffer_size(client, frames);
    }
    if (const auto get_latency = com_method<AudioGetStreamLatencyFn>(client_iface, 5)) {
        ReferenceTime latency = 0;
        ProbeCallScope scope;
        if (SUCCEEDED(get_latency(client_iface, &latency)))
            audioflex::timeline().on_stream_latency(client, latency);
    }
    if (const auto get_padding = com_method<AudioGetCurrentPaddingFn>(client_iface, 6)) {
        UINT32 frames = 0;
        ProbeCallScope scope;
        if (SUCCEEDED(get_padding(client_iface, &frames)))
            audioflex::timeline().on_padding(client, frames, get_timestamp());
    }
    if (const auto get_period = com_method<AudioGetDevicePeriodFn>(client_iface, 9)) {
        ReferenceTime default_period = 0;
        ReferenceTime min_period = 0;
        ProbeCallScope scope;
        if (SUCCEEDED(get_period(client_iface, &default_period, &min_period)))
            audioflex::timeline().on_device_period(client, default_period, min_period);
    }

    if (mode == audioflex::ShareMode::Shared && format) {
        void* client3 = nullptr;
        if (SUCCEEDED(query_interface(client_iface, IID_IAudioClient3_, &client3)) && client3) {
            publish_alias(reinterpret_cast<std::uintptr_t>(client3), client);
            if (const auto get_range = com_method<AudioGetSharedModeEnginePeriodFn>(client3, 18)) {
                UINT32 default_frames = 0;
                UINT32 fundamental_frames = 0;
                UINT32 min_frames = 0;
                UINT32 max_frames = 0;
                ProbeCallScope scope;
                if (SUCCEEDED(get_range(
                        client3, format, &default_frames, &fundamental_frames, &min_frames, &max_frames))) {
                    audioflex::timeline().on_period_range(
                        client, default_frames, fundamental_frames, min_frames, max_frames);
                    period_valid = true;
                }
            }
            if (g_co_task_mem_free) {
                if (const auto get_current = com_method<AudioGetCurrentSharedModeEnginePeriodFn>(client3, 19)) {
                    WaveFormatEx* current_format = nullptr;
                    UINT32 current_frames = 0;
                    HRESULT status = E_FAIL;
                    {
                        ProbeCallScope scope;
                        status = get_current(client3, &current_format, &current_frames);
                    }
                    if (SUCCEEDED(status)) {
                        audioflex::timeline().on_current_period(
                            client, current_frames,
                            current_format ? current_format->nSamplesPerSec : format->nSamplesPerSec);
                        period_valid = true;
                    }
                    if (current_format) g_co_task_mem_free(current_format);
                }
            }
            release(client3);
        }
    }

    if (audio_clock_enabled())
        probe_clock_once(client_iface, client, clock_valid);
    audioflex::timeline().on_active_probe(client, period_valid, clock_valid);

    const auto token = audioflex::timeline().client(client);
    spdlog::info(
        "AudioFlex active probe: client=0x{:x}, period_valid={}, clock_valid={}, buffer_frames={}, current_period_frames={}, stream_latency_us={}, mutation=false",
        client, period_valid, clock_valid,
        token ? token->buffer_frames : 0,
        token ? token->current_period_frames : 0,
        token ? audioflex::reference_time_to_ns(token->stream_latency_100ns) / 1000ull : 0ull);
}

void maybe_probe_runtime_clock(
    void* client_iface, std::uintptr_t client, std::uint64_t now_ns,
    const policy::AudioPolicy& audio) {
    if (!audio.active_probe || !audio.clock) return;
    const auto interval_ns = static_cast<std::uint64_t>(audio.clock_probe_interval_ms) * 1'000'000ull;
    if (!audioflex::timeline().should_probe_clock(client, now_ns, interval_ns)) return;
    bool clock_valid = false;
    probe_clock_once(client_iface, client, clock_valid);
}

const char* queue_confidence_name(audioflex::QueueModelConfidence confidence) noexcept {
    switch (confidence) {
        case audioflex::QueueModelConfidence::Padding: return "padding";
        case audioflex::QueueModelConfidence::Buffered: return "buffered";
        case audioflex::QueueModelConfidence::ClockCorrelated: return "clock-correlated";
        default: return "none";
    }
}

void maybe_log_queue_model(std::uintptr_t client, std::uint64_t now_ns) {
    // After the one-shot diagnostic has been emitted, avoid rebuilding a full
    // 20+ field queue snapshot on every GetCurrentPadding call.
    if (audioflex::timeline().queue_model_logged(client)) return;
    const auto estimate = audioflex::timeline().queue_estimate(client, now_ns);
    if (!estimate.valid || estimate.buffer_capacity_ns == 0) return;
    if (!audioflex::timeline().mark_queue_model_logged(client)) return;
    spdlog::info(
        "AudioFlex queue model active: client=0x{:x}, endpoint_queued_us={}, capacity_us={}, fill_permille={}, engine_period_us={}, stream_latency_us={}, clock_correlated={}, confidence={}, endpoint_queue_only=true",
        client,
        estimate.endpoint_queued_ns / 1000ull,
        estimate.buffer_capacity_ns / 1000ull,
        estimate.fill_permille,
        estimate.engine_period_ns / 1000ull,
        estimate.stream_latency_ns / 1000ull,
        estimate.clock_correlated,
        queue_confidence_name(estimate.confidence));
}

void maybe_observe_enumerator(REFCLSID clsid, REFIID iid, void* result) {
    if (!result || !guid_equal(clsid, CLSID_MMDeviceEnumerator_)) return;
    if (guid_equal(iid, IID_IMMDeviceEnumerator_)) {
        observe_enumerator(result);
        return;
    }
    void* enumerator = nullptr;
    if (SUCCEEDED(query_interface(result, IID_IMMDeviceEnumerator_, &enumerator)) && enumerator) {
        observe_enumerator(enumerator);
        release(enumerator);
    }
}

HRESULT WINAPI hkCoCreateInstance(REFCLSID clsid, void* outer, DWORD context, REFIID iid, void** result) {
    const auto status = g_co_create_instance
        ? g_co_create_instance(clsid, outer, context, iid, result)
        : E_FAIL;
    if (audio_enabled() && SUCCEEDED(status) && result && *result)
        maybe_observe_enumerator(clsid, iid, *result);
    return status;
}

HRESULT WINAPI hkEnumAudioEndpoints(void* self, DataFlow flow, DWORD state_mask, void** devices) {
    const auto status = g_enum_audio_endpoints
        ? g_enum_audio_endpoints(self, flow, state_mask, devices)
        : E_FAIL;
    if (audio_enabled() && SUCCEEDED(status) && devices && *devices)
        observe_collection(*devices, to_flow(flow));
    return status;
}

HRESULT WINAPI hkGetDefaultAudioEndpoint(void* self, DataFlow flow, int role, void** endpoint) {
    const auto status = g_get_default_audio_endpoint
        ? g_get_default_audio_endpoint(self, flow, role, endpoint)
        : E_FAIL;
    if (audio_enabled() && SUCCEEDED(status) && endpoint && *endpoint)
        observe_endpoint(*endpoint, to_flow(flow));
    return status;
}

HRESULT WINAPI hkGetDevice(void* self, LPCWSTR id, void** endpoint) {
    const auto status = g_get_device ? g_get_device(self, id, endpoint) : E_FAIL;
    if (audio_enabled() && SUCCEEDED(status) && endpoint && *endpoint)
        observe_endpoint(*endpoint, audioflex::StreamFlow::Unknown);
    return status;
}

HRESULT WINAPI hkCollectionItem(void* self, UINT index, void** endpoint) {
    const auto status = g_collection_item ? g_collection_item(self, index, endpoint) : E_FAIL;
    if (audio_enabled() && SUCCEEDED(status) && endpoint && *endpoint) {
        const auto flow = lookup_flow(g_collection_flows, reinterpret_cast<std::uintptr_t>(self));
        observe_endpoint(*endpoint, flow);
    }
    return status;
}

HRESULT WINAPI hkDeviceActivate(void* self, REFIID iid, DWORD context, void* params, void** interface_out) {
    const auto status = g_device_activate
        ? g_device_activate(self, iid, context, params, interface_out)
        : E_FAIL;
    if (!audio_enabled() || FAILED(status) || !interface_out || !*interface_out) return status;
    if (guid_equal(iid, IID_IAudioClient_) || guid_equal(iid, IID_IAudioClient2_) || guid_equal(iid, IID_IAudioClient3_)) {
        const auto flow = lookup_flow(g_endpoint_flows, reinterpret_cast<std::uintptr_t>(self));
        observe_audio_client(*interface_out, flow);
    }
    return status;
}

HRESULT WINAPI hkAudioInitialize(
    void* self, ShareMode mode, DWORD flags, ReferenceTime buffer_duration,
    ReferenceTime periodicity, const WaveFormatEx* format, const GUID* session_guid) {
    const auto status = g_audio_initialize
        ? g_audio_initialize(self, mode, flags, buffer_duration, periodicity, format, session_guid)
        : E_FAIL;
    if (audio_enabled() && SUCCEEDED(status)) {
        const auto client = canonical_client(self);
        const auto existing = audioflex::timeline().client(client);
        const auto flow = existing ? existing->flow : audioflex::StreamFlow::Unknown;
        audioflex::timeline().on_initialize(
            client, flow, to_share_mode(mode), flags,
            format ? format->nSamplesPerSec : 0,
            format ? format->nChannels : 0,
            format ? format->wBitsPerSample : 0,
            buffer_duration, periodicity);
        spdlog::info(
            "AudioFlex WASAPI stream initialized: client=0x{:x}, flow={}, mode={}, rate={}Hz, channels={}, bits={}, flags=0x{:x}, buffer_hns={}, periodicity_hns={}, observer_only=true",
            client, flow_name(flow), mode_name(to_share_mode(mode)),
            format ? format->nSamplesPerSec : 0,
            format ? format->nChannels : 0,
            format ? format->wBitsPerSample : 0,
            flags, buffer_duration, periodicity);
        probe_audio_client_after_initialize(self, client, to_share_mode(mode), format);
    }
    return status;
}

HRESULT WINAPI hkAudioGetBufferSize(void* self, UINT32* frames) {
    const auto status = g_audio_get_buffer_size ? g_audio_get_buffer_size(self, frames) : E_FAIL;
    if (!g_probe_call && audio_enabled() && SUCCEEDED(status) && frames) {
        const auto client = canonical_client(self);
        const auto before = audioflex::timeline().client(client);
        audioflex::timeline().on_buffer_size(client, *frames);
        if (!before || before->buffer_frames == 0) {
            const auto rate = before ? before->sample_rate : 0;
            spdlog::info(
                "AudioFlex buffer observed: client=0x{:x}, frames={}, rate={}Hz, duration_us={}",
                client, *frames, rate, audioflex::frames_to_ns(*frames, rate) / 1000ull);
        }
    }
    return status;
}

HRESULT WINAPI hkAudioGetStreamLatency(void* self, ReferenceTime* latency) {
    const auto status = g_audio_get_stream_latency ? g_audio_get_stream_latency(self, latency) : E_FAIL;
    if (!g_probe_call && audio_enabled() && SUCCEEDED(status) && latency)
        audioflex::timeline().on_stream_latency(canonical_client(self), *latency);
    return status;
}

HRESULT WINAPI hkAudioGetCurrentPadding(void* self, UINT32* frames) {
    const auto status = g_audio_get_current_padding ? g_audio_get_current_padding(self, frames) : E_FAIL;
    if (!g_probe_call && SUCCEEDED(status) && frames) {
        const auto& audio = Config::get().snapshot().audio;
        if (audio.enabled && audio.wasapi_observer) {
            const auto client = canonical_client(self);
            const auto now_ns = get_timestamp();
            audioflex::timeline().on_padding(client, *frames, now_ns);
            maybe_probe_runtime_clock(self, client, now_ns, audio);
            if (audio.queue_model) maybe_log_queue_model(client, now_ns);
        }
    }
    return status;
}

HRESULT WINAPI hkAudioGetDevicePeriod(void* self, ReferenceTime* default_period, ReferenceTime* min_period) {
    const auto status = g_audio_get_device_period
        ? g_audio_get_device_period(self, default_period, min_period)
        : E_FAIL;
    if (!g_probe_call && audio_enabled() && SUCCEEDED(status))
        audioflex::timeline().on_device_period(
            canonical_client(self), default_period ? *default_period : 0, min_period ? *min_period : 0);
    return status;
}

HRESULT WINAPI hkAudioStart(void* self) {
    const auto status = g_audio_start ? g_audio_start(self) : E_FAIL;
    if (audio_enabled() && SUCCEEDED(status)) audioflex::timeline().on_start(canonical_client(self));
    return status;
}

HRESULT WINAPI hkAudioStop(void* self) {
    const auto status = g_audio_stop ? g_audio_stop(self) : E_FAIL;
    if (audio_enabled() && SUCCEEDED(status)) audioflex::timeline().on_stop(canonical_client(self));
    return status;
}

HRESULT WINAPI hkAudioGetService(void* self, REFIID iid, void** service) {
    const auto status = g_audio_get_service ? g_audio_get_service(self, iid, service) : E_FAIL;
    if (!g_probe_call && audio_clock_enabled() && SUCCEEDED(status) && service && *service && guid_equal(iid, IID_IAudioClock_))
        observe_clock(*service, canonical_client(self));
    return status;
}

HRESULT WINAPI hkAudioGetSharedModeEnginePeriod(
    void* self, const WaveFormatEx* format, UINT32* default_frames, UINT32* fundamental_frames,
    UINT32* min_frames, UINT32* max_frames) {
    const auto status = g_audio_get_shared_period
        ? g_audio_get_shared_period(self, format, default_frames, fundamental_frames, min_frames, max_frames)
        : E_FAIL;
    if (!g_probe_call && audio_enabled() && SUCCEEDED(status) && default_frames && fundamental_frames && min_frames && max_frames) {
        const auto client = canonical_client(self);
        audioflex::timeline().on_period_range(client, *default_frames, *fundamental_frames, *min_frames, *max_frames);
        spdlog::debug(
            "AudioFlex shared engine period range: client=0x{:x}, rate={}Hz, default={}, fundamental={}, min={}, max={}",
            client, format ? format->nSamplesPerSec : 0,
            *default_frames, *fundamental_frames, *min_frames, *max_frames);
    }
    return status;
}

HRESULT WINAPI hkAudioGetCurrentSharedModeEnginePeriod(void* self, WaveFormatEx** format, UINT32* current_frames) {
    const auto status = g_audio_get_current_period
        ? g_audio_get_current_period(self, format, current_frames)
        : E_FAIL;
    if (!g_probe_call && audio_enabled() && SUCCEEDED(status) && current_frames) {
        const auto rate = format && *format ? (*format)->nSamplesPerSec : 0;
        audioflex::timeline().on_current_period(canonical_client(self), *current_frames, rate);
    }
    return status;
}

HRESULT WINAPI hkAudioInitializeSharedAudioStream(
    void* self, DWORD flags, UINT32 period_frames, const WaveFormatEx* format, const GUID* session_guid) {
    if (!g_audio_initialize_shared) return E_FAIL;

    UINT32 effective_period_frames = period_frames;
    bool negotiated = false;
    audioflex::AudioPeriodDecision decision{};

    const auto mode = audio_adaptive_period_mode();
    if (audio_enabled() && mode != policy::AutoBool::Disabled && format && g_audio_get_shared_period) {
        UINT32 default_frames = 0;
        UINT32 fundamental_frames = 0;
        UINT32 min_frames = 0;
        UINT32 max_frames = 0;
        HRESULT range_status = E_FAIL;
        {
            ProbeCallScope scope;
            range_status = g_audio_get_shared_period(
                self, format, &default_frames, &fundamental_frames, &min_frames, &max_frames);
        }
        if (SUCCEEDED(range_status)) {
            const auto& cfg = Config::get().snapshot().audio;
            decision = audioflex::choose_period_candidate(
                {.sample_rate = format->nSamplesPerSec,
                 .requested_frames = period_frames,
                 .default_frames = default_frames,
                 .fundamental_frames = fundamental_frames,
                 .min_frames = min_frames,
                 .max_frames = max_frames},
                cfg.period_target_us, cfg.period_max_reduction_percent);

            if (decision.valid && decision.change) {
                const auto candidate_us = audioflex::frames_to_ns(
                    decision.candidate_frames, format->nSamplesPerSec) / 1000ull;
                if (mode == policy::AutoBool::Auto) {
                    spdlog::info(
                        "AudioFlex adaptive-period candidate: requested_frames={}, candidate_frames={}, candidate_us={}, target_us={}, range={}/{}/{}/{}, action=recommend-only",
                        period_frames, decision.candidate_frames, candidate_us, cfg.period_target_us,
                        default_frames, fundamental_frames, min_frames, max_frames);
                } else {
                    effective_period_frames = decision.candidate_frames;
                    negotiated = true;
                    spdlog::info(
                        "AudioFlex adaptive-period negotiation: requested_frames={}, candidate_frames={}, candidate_us={}, target_us={}, range={}/{}/{}/{}, action=try",
                        period_frames, effective_period_frames, candidate_us, cfg.period_target_us,
                        default_frames, fundamental_frames, min_frames, max_frames);
                }
            }
        }
    }

    const auto status = g_audio_initialize_shared(self, flags, effective_period_frames, format, session_guid);
    if (FAILED(status) && negotiated) {
        spdlog::warn(
            "AudioFlex adaptive-period initialization failed: requested_frames={}, candidate_frames={}, hr=0x{:08x}; no automatic retry on the same IAudioClient",
            period_frames, effective_period_frames, static_cast<unsigned>(status));
    }

    if (audio_enabled() && SUCCEEDED(status)) {
        const auto client = canonical_client(self);
        const auto existing = audioflex::timeline().client(client);
        const auto flow = existing ? existing->flow : audioflex::StreamFlow::Unknown;
        audioflex::timeline().on_initialize(
            client, flow, audioflex::ShareMode::Shared, flags,
            format ? format->nSamplesPerSec : 0,
            format ? format->nChannels : 0,
            format ? format->wBitsPerSample : 0,
            0, 0, true, effective_period_frames);
        spdlog::info(
            "AudioFlex IAudioClient3 shared stream initialized: client=0x{:x}, flow={}, rate={}Hz, channels={}, bits={}, requested_period_frames={}, effective_period_frames={}, negotiated={}, flags=0x{:x}",
            client, flow_name(flow), format ? format->nSamplesPerSec : 0,
            format ? format->nChannels : 0, format ? format->wBitsPerSample : 0,
            period_frames, effective_period_frames, negotiated, flags);
        probe_audio_client_after_initialize(self, client, audioflex::ShareMode::Shared, format);
    }
    return status;
}

HRESULT WINAPI hkAudioClockGetFrequency(void* self, UINT64* frequency) {
    const auto status = g_clock_get_frequency ? g_clock_get_frequency(self, frequency) : E_FAIL;
    if (!g_probe_call && audio_enabled() && SUCCEEDED(status) && frequency)
        audioflex::timeline().on_clock_frequency(reinterpret_cast<std::uintptr_t>(self), *frequency);
    return status;
}

HRESULT WINAPI hkAudioClockGetPosition(void* self, UINT64* position, UINT64* qpc_position_100ns) {
    const auto status = g_clock_get_position ? g_clock_get_position(self, position, qpc_position_100ns) : E_FAIL;
    if (!g_probe_call && audio_enabled() && SUCCEEDED(status) && position) {
        const auto qpc_host_ns = qpc_position_100ns
            ? audioflex::hundred_ns_to_ns(*qpc_position_100ns)
            : 0;
        audioflex::timeline().on_clock_position(
            reinterpret_cast<std::uintptr_t>(self), *position, qpc_host_ns, get_timestamp());
    }
    return status;
}

} // namespace

void AudioHooks::initialize() {
    if (!audio_enabled()) return;

    std::scoped_lock lock(g_hook_mutex);
    if (g_initialized) return;

    auto ole32 = GetModuleHandleA("ole32.dll");
    if (!ole32) {
        spdlog::warn("AudioFlex unavailable: ole32.dll is not loaded");
        return;
    }

    g_co_task_mem_free = reinterpret_cast<CoTaskMemFreeFn>(GetProcAddress(ole32, "CoTaskMemFree"));

    auto target = reinterpret_cast<void*>(GetProcAddress(ole32, "CoCreateInstance"));
    if (!target) {
        spdlog::warn("AudioFlex unavailable: ole32.dll has no CoCreateInstance export");
        return;
    }

    if (!attach_once(g_co_create_instance, g_co_attached, target,
                     reinterpret_cast<void*>(hkCoCreateInstance), "CoCreateInstance"))
        return;

    g_initialized = true;
    const auto& audio = Config::get().snapshot().audio;
    spdlog::info(
        "AudioFlex WASAPI observer armed: discovery=MMDeviceEnumerator, active_probe={}, queue_model={}, clock_probe_interval_ms={}, adaptive_period={}, period_target_us={}, mutation=init-time-opt-in",
        audio.active_probe, audio.queue_model, audio.clock_probe_interval_ms,
        audio.adaptive_period == policy::AutoBool::Auto ? "auto" :
            (audio.adaptive_period == policy::AutoBool::Enabled ? "enabled" : "disabled"),
        audio.period_target_us);
}

void AudioHooks::shutdown() {
    std::scoped_lock lock(g_hook_mutex);
    if (!g_initialized && !g_co_attached) return;

    const auto stats = audioflex::timeline().stats();
    const auto queue_avg_us = stats.queue_model_samples
        ? (stats.queue_total_ns / stats.queue_model_samples) / 1000ull
        : 0ull;
    spdlog::info(
        "AudioFlex shutdown: clients={}, init={}, starts={}, stops={}, padding_samples={}, period_samples={}, clock_bindings={}, clock_samples={}, active_probes={}, probe_failures={}, queue_samples={}, queue_avg_us={}, queue_max_us={}, dropped={}",
        stats.clients, stats.initializations, stats.starts, stats.stops,
        stats.padding_samples, stats.period_samples, stats.clock_bindings,
        stats.clock_samples, stats.active_probes, stats.active_probe_failures,
        stats.queue_model_samples, queue_avg_us, stats.queue_max_ns / 1000ull, stats.dropped);

    detach_if(g_clock_get_position, g_clock_position_attached, reinterpret_cast<void*>(hkAudioClockGetPosition));
    detach_if(g_clock_get_frequency, g_clock_frequency_attached, reinterpret_cast<void*>(hkAudioClockGetFrequency));
    detach_if(g_audio_initialize_shared, g_initialize_shared_attached, reinterpret_cast<void*>(hkAudioInitializeSharedAudioStream));
    detach_if(g_audio_get_current_period, g_current_period_attached, reinterpret_cast<void*>(hkAudioGetCurrentSharedModeEnginePeriod));
    detach_if(g_audio_get_shared_period, g_shared_period_attached, reinterpret_cast<void*>(hkAudioGetSharedModeEnginePeriod));
    detach_if(g_audio_get_service, g_service_attached, reinterpret_cast<void*>(hkAudioGetService));
    detach_if(g_audio_stop, g_stop_attached, reinterpret_cast<void*>(hkAudioStop));
    detach_if(g_audio_start, g_start_attached, reinterpret_cast<void*>(hkAudioStart));
    detach_if(g_audio_get_device_period, g_device_period_attached, reinterpret_cast<void*>(hkAudioGetDevicePeriod));
    detach_if(g_audio_get_current_padding, g_padding_attached, reinterpret_cast<void*>(hkAudioGetCurrentPadding));
    detach_if(g_audio_get_stream_latency, g_stream_latency_attached, reinterpret_cast<void*>(hkAudioGetStreamLatency));
    detach_if(g_audio_get_buffer_size, g_buffer_size_attached, reinterpret_cast<void*>(hkAudioGetBufferSize));
    detach_if(g_audio_initialize, g_initialize_attached, reinterpret_cast<void*>(hkAudioInitialize));
    detach_if(g_device_activate, g_activate_attached, reinterpret_cast<void*>(hkDeviceActivate));
    detach_if(g_collection_item, g_collection_item_attached, reinterpret_cast<void*>(hkCollectionItem));
    detach_if(g_get_device, g_get_device_attached, reinterpret_cast<void*>(hkGetDevice));
    detach_if(g_get_default_audio_endpoint, g_default_attached, reinterpret_cast<void*>(hkGetDefaultAudioEndpoint));
    detach_if(g_enum_audio_endpoints, g_enum_attached, reinterpret_cast<void*>(hkEnumAudioEndpoints));
    detach_if(g_co_create_instance, g_co_attached, reinterpret_cast<void*>(hkCoCreateInstance));

    g_initialized = false;
}
