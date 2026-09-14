#include "openxr_hooks.h"

#include "config.h"
#include "fakenvapi.h"
#include "log.h"
#include "util.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>

#include <detours.h>

namespace {
std::mutex g_openxr_hook_mutex;
HMODULE g_hooked_openxr_module = nullptr;
bool g_openxr_hooks_installed = false;

xrabi::PFN_xrGetInstanceProcAddr g_xr_get_instance_proc_addr = nullptr;
xrabi::PFN_xrCreateSession g_xr_create_session = nullptr;
xrabi::PFN_xrWaitFrame g_xr_wait_frame = nullptr;
xrabi::PFN_xrBeginFrame g_xr_begin_frame = nullptr;
xrabi::PFN_xrEndFrame g_xr_end_frame = nullptr;
xrabi::PFN_xrDestroySession g_xr_destroy_session = nullptr;

std::atomic<xrabi::PFN_xrCreateSession> g_dispatch_create_session{nullptr};
std::atomic<xrabi::PFN_xrWaitFrame> g_dispatch_wait_frame{nullptr};
std::atomic<xrabi::PFN_xrBeginFrame> g_dispatch_begin_frame{nullptr};
std::atomic<xrabi::PFN_xrEndFrame> g_dispatch_end_frame{nullptr};
std::atomic<xrabi::PFN_xrDestroySession> g_dispatch_destroy_session{nullptr};

bool g_gipa_attached = false;
bool g_create_session_attached = false;
bool g_wait_attached = false;
bool g_begin_attached = false;
bool g_end_attached = false;
bool g_destroy_session_attached = false;

constexpr std::size_t kClockBindingSlots = 16;
constexpr std::uintptr_t kClockBindingBusy = ~std::uintptr_t{0};

struct ClockBinding {
    std::atomic<std::uintptr_t> session{0}; // ownership/publication word
    // revision is a tiny seqlock for payload replacement. It also closes the
    // same-handle ABA window if an OpenXR runtime recycles an XrSession value.
    std::atomic<std::uint64_t> revision{0};
    std::atomic<xrabi::XrInstance> instance{nullptr};
    std::atomic<xrabi::PFN_xrConvertTimeToWin32PerformanceCounterKHR> convert{nullptr};
    // Wait and End are separately serialized by the OpenXR frame-loop rules.
    // Keep their telemetry local so successful conversion does not issue a
    // globally contended locked RMW twice per XR frame.
    std::atomic<std::uint64_t> wait_conversions{0};
    std::atomic<std::uint64_t> end_conversions{0};
    std::atomic<std::uint64_t> wait_failures{0};
    std::atomic<std::uint64_t> end_failures{0};
};

std::array<ClockBinding, kClockBindingSlots> g_clock_bindings{};
std::atomic<std::uint64_t> g_clock_sessions{0};
std::atomic<std::uint64_t> g_clock_available{0};
std::atomic<std::uint64_t> g_clock_conversions{0};
std::atomic<std::uint64_t> g_clock_failures{0};

inline void serial_increment(std::atomic<std::uint64_t>& counter) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

enum class ClockSampleKind : std::uint8_t { Wait, End };

ClockBinding* find_clock_binding(std::uintptr_t session) noexcept {
    if (session == 0) return nullptr;

    struct ThreadCache {
        std::uintptr_t session = 0;
        ClockBinding* binding = nullptr;
    };
    static thread_local ThreadCache cache;
    if (cache.session == session && cache.binding &&
        cache.binding->session.load(std::memory_order_acquire) == session)
        return cache.binding;

    const auto start = (session >> 4) % kClockBindingSlots;
    for (std::size_t probe = 0; probe < kClockBindingSlots; ++probe) {
        auto& slot = g_clock_bindings[(start + probe) % kClockBindingSlots];
        const auto key = slot.session.load(std::memory_order_acquire);
        if (key == session) {
            cache = {session, &slot};
            return &slot;
        }
        // Bindings use open addressing and are erased on session destruction,
        // so holes can exist before colliding live keys. Do not early-out.
    }
    return nullptr;
}

void publish_clock_binding(
    xrabi::XrSession session_handle, xrabi::XrInstance instance,
    xrabi::PFN_xrConvertTimeToWin32PerformanceCounterKHR convert) noexcept {
    const auto session = reinterpret_cast<std::uintptr_t>(session_handle);
    if (session == 0) return;
    const auto start = (session >> 4) % kClockBindingSlots;
    for (std::size_t probe = 0; probe < kClockBindingSlots; ++probe) {
        auto& slot = g_clock_bindings[(start + probe) % kClockBindingSlots];
        const auto key = slot.session.load(std::memory_order_acquire);
        if (key == session) {
            auto revision = slot.revision.load(std::memory_order_relaxed);
            if (revision & 1u) ++revision;
            slot.revision.store(revision + 1, std::memory_order_release);
            slot.instance.store(instance, std::memory_order_relaxed);
            slot.convert.store(convert, std::memory_order_relaxed);
            slot.revision.store(revision + 2, std::memory_order_release);
            return;
        }
        if (key != 0) continue;
        std::uintptr_t expected = 0;
        if (!slot.session.compare_exchange_strong(
                expected, kClockBindingBusy, std::memory_order_acq_rel,
                std::memory_order_acquire))
            continue;
        auto revision = slot.revision.load(std::memory_order_relaxed);
        if (revision & 1u) ++revision;
        slot.revision.store(revision + 1, std::memory_order_relaxed);
        slot.instance.store(instance, std::memory_order_relaxed);
        slot.convert.store(convert, std::memory_order_relaxed);
        slot.wait_conversions.store(0, std::memory_order_relaxed);
        slot.end_conversions.store(0, std::memory_order_relaxed);
        slot.wait_failures.store(0, std::memory_order_relaxed);
        slot.end_failures.store(0, std::memory_order_relaxed);
        slot.revision.store(revision + 2, std::memory_order_relaxed);
        slot.session.store(session, std::memory_order_release);
        g_clock_sessions.fetch_add(1, std::memory_order_relaxed);
        if (convert) g_clock_available.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_clock_failures.fetch_add(1, std::memory_order_relaxed);
}

void clear_clock_binding(xrabi::XrSession session_handle) noexcept {
    const auto session = reinterpret_cast<std::uintptr_t>(session_handle);
    if (auto* slot = find_clock_binding(session)) {
        g_clock_conversions.fetch_add(
            slot->wait_conversions.load(std::memory_order_relaxed) +
            slot->end_conversions.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        g_clock_failures.fetch_add(
            slot->wait_failures.load(std::memory_order_relaxed) +
            slot->end_failures.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        auto revision = slot->revision.load(std::memory_order_relaxed);
        if (revision & 1u) ++revision;
        slot->revision.store(revision + 1, std::memory_order_release);
        slot->convert.store(nullptr, std::memory_order_relaxed);
        slot->instance.store(nullptr, std::memory_order_relaxed);
        slot->revision.store(revision + 2, std::memory_order_release);
        slot->session.store(0, std::memory_order_release);
    }
}

struct ClockDispatchSnapshot {
    ClockBinding* binding = nullptr;
    std::uintptr_t session = 0;
    std::uint64_t revision = 0;
    xrabi::XrInstance instance = nullptr;
    xrabi::PFN_xrConvertTimeToWin32PerformanceCounterKHR convert = nullptr;
};

std::optional<ClockDispatchSnapshot> clock_dispatch_for(std::uintptr_t session) noexcept {
    auto* binding = find_clock_binding(session);
    if (!binding) return std::nullopt;

    const auto key = binding->session.load(std::memory_order_acquire);
    if (key != session) return std::nullopt;
    const auto revision = binding->revision.load(std::memory_order_acquire);
    if (revision & 1u) return std::nullopt;

    ClockDispatchSnapshot snapshot{};
    snapshot.binding = binding;
    snapshot.session = session;
    snapshot.revision = revision;
    snapshot.instance = binding->instance.load(std::memory_order_relaxed);
    snapshot.convert = binding->convert.load(std::memory_order_relaxed);

    if (binding->revision.load(std::memory_order_acquire) != revision ||
        binding->session.load(std::memory_order_acquire) != session)
        return std::nullopt;
    if (!snapshot.instance || !snapshot.convert) return std::nullopt;
    return snapshot;
}

std::optional<std::uint64_t> xr_time_to_host_ns(
    xrabi::XrSession session_handle, xrabi::XrTime time, bool clock_sync,
    ClockSampleKind kind) noexcept {
    if (!clock_sync || time <= 0) return std::nullopt;
    const auto session = reinterpret_cast<std::uintptr_t>(session_handle);
    const auto dispatch = clock_dispatch_for(session);
    if (!dispatch) return std::nullopt;

    LARGE_INTEGER counter{};
    const auto result = dispatch->convert(dispatch->instance, time, &counter);
    const auto still_current = [&]() noexcept {
        return dispatch->binding &&
            dispatch->binding->session.load(std::memory_order_acquire) == dispatch->session &&
            dispatch->binding->revision.load(std::memory_order_acquire) == dispatch->revision;
    };
    if (!xrabi::succeeded(result)) {
        if (still_current()) {
            auto& failures = kind == ClockSampleKind::Wait
                ? dispatch->binding->wait_failures : dispatch->binding->end_failures;
            serial_increment(failures);
        } else {
            g_clock_failures.fetch_add(1, std::memory_order_relaxed);
        }
        return std::nullopt;
    }
    const auto host_ns = qpc_ticks_to_ns(counter.QuadPart);
    if (host_ns == 0) {
        if (still_current()) {
            auto& failures = kind == ClockSampleKind::Wait
                ? dispatch->binding->wait_failures : dispatch->binding->end_failures;
            serial_increment(failures);
        } else {
            g_clock_failures.fetch_add(1, std::memory_order_relaxed);
        }
        return std::nullopt;
    }
    if (still_current()) {
        auto& conversions = kind == ClockSampleKind::Wait
            ? dispatch->binding->wait_conversions : dispatch->binding->end_conversions;
        serial_increment(conversions);
    } else {
        // Extremely rare teardown/reuse race: preserve statistics without
        // touching a newly published session generation.
        g_clock_conversions.fetch_add(1, std::memory_order_relaxed);
    }
    return host_ns;
}

template <typename T>
T downstream(T direct, const std::atomic<T>& indirect) noexcept {
    return direct ? direct : indirect.load(std::memory_order_acquire);
}

void publish_indirect(const char* name, xrabi::PFN_xrVoidFunction function) noexcept {
    if (!name || !function) return;
    if (std::strcmp(name, "xrCreateSession") == 0) {
        g_dispatch_create_session.store(
            reinterpret_cast<xrabi::PFN_xrCreateSession>(function), std::memory_order_release);
    } else if (std::strcmp(name, "xrWaitFrame") == 0) {
        g_dispatch_wait_frame.store(
            reinterpret_cast<xrabi::PFN_xrWaitFrame>(function), std::memory_order_release);
    } else if (std::strcmp(name, "xrBeginFrame") == 0) {
        g_dispatch_begin_frame.store(
            reinterpret_cast<xrabi::PFN_xrBeginFrame>(function), std::memory_order_release);
    } else if (std::strcmp(name, "xrEndFrame") == 0) {
        g_dispatch_end_frame.store(
            reinterpret_cast<xrabi::PFN_xrEndFrame>(function), std::memory_order_release);
    } else if (std::strcmp(name, "xrDestroySession") == 0) {
        g_dispatch_destroy_session.store(
            reinterpret_cast<xrabi::PFN_xrDestroySession>(function), std::memory_order_release);
    }
}

xrabi::PFN_xrVoidFunction hook_for_name(const char* name) noexcept {
    if (!name) return nullptr;
    if (std::strcmp(name, "xrCreateSession") == 0)
        return reinterpret_cast<xrabi::PFN_xrVoidFunction>(OpenXRHooks::hkxrCreateSession);
    if (std::strcmp(name, "xrWaitFrame") == 0)
        return reinterpret_cast<xrabi::PFN_xrVoidFunction>(OpenXRHooks::hkxrWaitFrame);
    if (std::strcmp(name, "xrBeginFrame") == 0)
        return reinterpret_cast<xrabi::PFN_xrVoidFunction>(OpenXRHooks::hkxrBeginFrame);
    if (std::strcmp(name, "xrEndFrame") == 0)
        return reinterpret_cast<xrabi::PFN_xrVoidFunction>(OpenXRHooks::hkxrEndFrame);
    if (std::strcmp(name, "xrDestroySession") == 0)
        return reinterpret_cast<xrabi::PFN_xrVoidFunction>(OpenXRHooks::hkxrDestroySession);
    return nullptr;
}
} // namespace

void OpenXRHooks::initialize(HMODULE openxr_module) {
    if (!Config::get().snapshot().openxr.enabled)
        return;

    if (!openxr_module)
        openxr_module = GetModuleHandleA("openxr_loader.dll");
    if (!openxr_module)
        return;

    std::scoped_lock lock(g_openxr_hook_mutex);
    if (g_openxr_hooks_installed && g_hooked_openxr_module == openxr_module)
        return;
    if (g_openxr_hooks_installed) {
        spdlog::warn("XRFlex observed a second openxr_loader.dll; keeping the first loader");
        return;
    }

    g_xr_get_instance_proc_addr = reinterpret_cast<xrabi::PFN_xrGetInstanceProcAddr>(
        GetProcAddress(openxr_module, "xrGetInstanceProcAddr"));
    g_xr_create_session = reinterpret_cast<xrabi::PFN_xrCreateSession>(
        GetProcAddress(openxr_module, "xrCreateSession"));
    g_xr_wait_frame = reinterpret_cast<xrabi::PFN_xrWaitFrame>(
        GetProcAddress(openxr_module, "xrWaitFrame"));
    g_xr_begin_frame = reinterpret_cast<xrabi::PFN_xrBeginFrame>(
        GetProcAddress(openxr_module, "xrBeginFrame"));
    g_xr_end_frame = reinterpret_cast<xrabi::PFN_xrEndFrame>(
        GetProcAddress(openxr_module, "xrEndFrame"));
    g_xr_destroy_session = reinterpret_cast<xrabi::PFN_xrDestroySession>(
        GetProcAddress(openxr_module, "xrDestroySession"));

    if (!g_xr_get_instance_proc_addr) {
        spdlog::warn("XRFlex unavailable: openxr_loader.dll has no xrGetInstanceProcAddr export");
        return;
    }

    if (DetourTransactionBegin() != NO_ERROR)
        return;
    DetourUpdateThread(GetCurrentThread());

    if (DetourAttach(
            reinterpret_cast<PVOID*>(&g_xr_get_instance_proc_addr),
            reinterpret_cast<PVOID>(hkxrGetInstanceProcAddr)) == NO_ERROR)
        g_gipa_attached = true;

    if (g_xr_create_session && DetourAttach(
            reinterpret_cast<PVOID*>(&g_xr_create_session),
            reinterpret_cast<PVOID>(hkxrCreateSession)) == NO_ERROR)
        g_create_session_attached = true;

    if (g_xr_wait_frame && DetourAttach(
            reinterpret_cast<PVOID*>(&g_xr_wait_frame),
            reinterpret_cast<PVOID>(hkxrWaitFrame)) == NO_ERROR)
        g_wait_attached = true;
    if (g_xr_begin_frame && DetourAttach(
            reinterpret_cast<PVOID*>(&g_xr_begin_frame),
            reinterpret_cast<PVOID>(hkxrBeginFrame)) == NO_ERROR)
        g_begin_attached = true;
    if (g_xr_end_frame && DetourAttach(
            reinterpret_cast<PVOID*>(&g_xr_end_frame),
            reinterpret_cast<PVOID>(hkxrEndFrame)) == NO_ERROR)
        g_end_attached = true;
    if (g_xr_destroy_session && DetourAttach(
            reinterpret_cast<PVOID*>(&g_xr_destroy_session),
            reinterpret_cast<PVOID>(hkxrDestroySession)) == NO_ERROR)
        g_destroy_session_attached = true;

    if (DetourTransactionCommit() != NO_ERROR) {
        g_gipa_attached = false;
        g_create_session_attached = false;
        g_wait_attached = false;
        g_begin_attached = false;
        g_end_attached = false;
        g_destroy_session_attached = false;
        spdlog::error("XRFlex OpenXR hook transaction failed");
        return;
    }

    g_hooked_openxr_module = openxr_module;
    g_openxr_hooks_installed = true;
    spdlog::info(
        "XRFlex OpenXR hooks installed: gipa=true, create_session={}, wait={}, begin={}, end={}, "
        "destroy_session={}, clock_sync={}, observer_only=true",
        g_create_session_attached, g_wait_attached, g_begin_attached, g_end_attached,
        g_destroy_session_attached, Config::get().snapshot().openxr.clock_sync);
}

void OpenXRHooks::shutdown() {
    std::scoped_lock lock(g_openxr_hook_mutex);
    if (!g_openxr_hooks_installed)
        return;

    if (auto* ctx = LowLatencyCtx::get()) {
        const auto stats = ctx->OpenXRStats();
        spdlog::info(
            "XRFlex stats: waits={}, begins={}, ends={}, discarded={}, correlated={}, clocked_waits={}, "
            "clocked_ends={}, dropped={}",
            stats.waits, stats.begins, stats.ends, stats.discarded, stats.correlated,
            stats.clocked_waits, stats.clocked_ends, stats.dropped);
        auto conversions = g_clock_conversions.load(std::memory_order_relaxed);
        auto failures = g_clock_failures.load(std::memory_order_relaxed);
        for (const auto& slot : g_clock_bindings) {
            const auto key = slot.session.load(std::memory_order_acquire);
            if (key == 0 || key == kClockBindingBusy) continue;
            conversions += slot.wait_conversions.load(std::memory_order_relaxed) +
                slot.end_conversions.load(std::memory_order_relaxed);
            failures += slot.wait_failures.load(std::memory_order_relaxed) +
                slot.end_failures.load(std::memory_order_relaxed);
        }
        spdlog::info(
            "XRFlex clock stats: sessions={}, available={}, conversions={}, failures={}, host_clock=qpc",
            g_clock_sessions.load(std::memory_order_relaxed),
            g_clock_available.load(std::memory_order_relaxed),
            conversions, failures);
    }

    if (DetourTransactionBegin() != NO_ERROR)
        return;
    DetourUpdateThread(GetCurrentThread());

    if (g_gipa_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&g_xr_get_instance_proc_addr), reinterpret_cast<PVOID>(hkxrGetInstanceProcAddr));
    if (g_create_session_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&g_xr_create_session), reinterpret_cast<PVOID>(hkxrCreateSession));
    if (g_wait_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&g_xr_wait_frame), reinterpret_cast<PVOID>(hkxrWaitFrame));
    if (g_begin_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&g_xr_begin_frame), reinterpret_cast<PVOID>(hkxrBeginFrame));
    if (g_end_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&g_xr_end_frame), reinterpret_cast<PVOID>(hkxrEndFrame));
    if (g_destroy_session_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&g_xr_destroy_session), reinterpret_cast<PVOID>(hkxrDestroySession));

    if (DetourTransactionCommit() == NO_ERROR) {
        g_openxr_hooks_installed = false;
        g_hooked_openxr_module = nullptr;
        g_gipa_attached = false;
        g_create_session_attached = false;
        g_wait_attached = false;
        g_begin_attached = false;
        g_end_attached = false;
        g_destroy_session_attached = false;
        g_dispatch_create_session.store(nullptr, std::memory_order_release);
        g_dispatch_wait_frame.store(nullptr, std::memory_order_release);
        g_dispatch_begin_frame.store(nullptr, std::memory_order_release);
        g_dispatch_end_frame.store(nullptr, std::memory_order_release);
        g_dispatch_destroy_session.store(nullptr, std::memory_order_release);
        for (auto& slot : g_clock_bindings) {
            slot.convert.store(nullptr, std::memory_order_relaxed);
            slot.instance.store(nullptr, std::memory_order_relaxed);
            slot.wait_conversions.store(0, std::memory_order_relaxed);
            slot.end_conversions.store(0, std::memory_order_relaxed);
            slot.wait_failures.store(0, std::memory_order_relaxed);
            slot.end_failures.store(0, std::memory_order_relaxed);
            slot.revision.fetch_add(2, std::memory_order_relaxed);
            slot.session.store(0, std::memory_order_release);
        }
    }
}

xrabi::XrResult WINAPI OpenXRHooks::hkxrGetInstanceProcAddr(
    xrabi::XrInstance instance,
    const char* name,
    xrabi::PFN_xrVoidFunction* function) {
    if (!g_xr_get_instance_proc_addr)
        return xrabi::XR_ERROR_RUNTIME_FAILURE;

    const auto result = g_xr_get_instance_proc_addr(instance, name, function);
    if (!xrabi::succeeded(result) || !function || !*function)
        return result;

    // Preserve the loader/runtime's actual downstream pointer before returning
    // our local observer entry point. This covers application-managed dispatch
    // tables without relying only on direct exported loader trampolines.
    publish_indirect(name, *function);
    if (Config::get().snapshot().openxr.enabled) {
        if (const auto hook = hook_for_name(name))
            *function = hook;
    }
    return result;
}

xrabi::XrResult WINAPI OpenXRHooks::hkxrCreateSession(
    xrabi::XrInstance instance,
    const xrabi::XrSessionCreateInfo* create_info,
    xrabi::XrSession* session) {
    const auto fn = downstream(g_xr_create_session, g_dispatch_create_session);
    if (!fn) return xrabi::XR_ERROR_RUNTIME_FAILURE;
    const auto result = fn(instance, create_info, session);
    if (!xrabi::succeeded(result) || !session || !*session) return result;

    // Resolve the conversion command on the concrete instance. Per OpenXR
    // xrGetInstanceProcAddr semantics, a non-null extension function pointer
    // here means the extension is enabled for this instance; we never enable
    // or inject the extension ourselves.
    xrabi::PFN_xrVoidFunction raw = nullptr;
    xrabi::PFN_xrConvertTimeToWin32PerformanceCounterKHR convert = nullptr;
    if (g_xr_get_instance_proc_addr) {
        const auto clock_result = g_xr_get_instance_proc_addr(
            instance, "xrConvertTimeToWin32PerformanceCounterKHR", &raw);
        if (xrabi::succeeded(clock_result) && raw) {
            convert = reinterpret_cast<xrabi::PFN_xrConvertTimeToWin32PerformanceCounterKHR>(raw);
        }
    }
    publish_clock_binding(*session, instance, convert);
    spdlog::info(
        "XRFlex session clock capability: session=0x{:x}, "
        "XR_KHR_win32_convert_performance_counter_time={}, host_clock=qpc",
        reinterpret_cast<std::uintptr_t>(*session), convert != nullptr);
    return result;
}

xrabi::XrResult WINAPI OpenXRHooks::hkxrWaitFrame(
    xrabi::XrSession session,
    const xrabi::XrFrameWaitInfo* frame_wait_info,
    xrabi::XrFrameState* frame_state) {
    const auto fn = downstream(g_xr_wait_frame, g_dispatch_wait_frame);
    if (!fn) return xrabi::XR_ERROR_RUNTIME_FAILURE;

    // The runtime remains the only XR timing owner. We call it first and only
    // observe the returned prediction after its own synchronization/throttling.
    const auto result = fn(session, frame_wait_info, frame_state);
    const auto return_timestamp_ns = get_timestamp();
    if (xrabi::succeeded(result) && frame_state) {
        const auto& xr = Config::get().snapshot().openxr;
        if (xr.enabled && xr.timeline) {
            if (auto* ctx = LowLatencyCtx::get()) {
                const auto predicted_host_ns = xr_time_to_host_ns(
                    session, frame_state->predictedDisplayTime, xr.clock_sync, ClockSampleKind::Wait);
                ctx->OpenXROnWaitFrame(
                    reinterpret_cast<std::uintptr_t>(session),
                    frame_state->predictedDisplayTime,
                    frame_state->predictedDisplayPeriod,
                    frame_state->shouldRender != 0,
                    return_timestamp_ns,
                    predicted_host_ns);
            }
        }
    }
    return result;
}

xrabi::XrResult WINAPI OpenXRHooks::hkxrBeginFrame(
    xrabi::XrSession session,
    const xrabi::XrFrameBeginInfo* frame_begin_info) {
    const auto fn = downstream(g_xr_begin_frame, g_dispatch_begin_frame);
    if (!fn) return xrabi::XR_ERROR_RUNTIME_FAILURE;
    const auto result = fn(session, frame_begin_info);
    const auto return_timestamp_ns = get_timestamp();
    if (xrabi::succeeded(result)) {
        const auto& xr = Config::get().snapshot().openxr;
        if (xr.enabled && xr.timeline) {
            if (auto* ctx = LowLatencyCtx::get()) {
                ctx->OpenXROnBeginFrame(
                    reinterpret_cast<std::uintptr_t>(session),
                    result == xrabi::XR_FRAME_DISCARDED,
                    return_timestamp_ns);
            }
        }
    }
    return result;
}

xrabi::XrResult WINAPI OpenXRHooks::hkxrEndFrame(
    xrabi::XrSession session,
    const xrabi::XrFrameEndInfo* frame_end_info) {
    const auto fn = downstream(g_xr_end_frame, g_dispatch_end_frame);
    if (!fn) return xrabi::XR_ERROR_RUNTIME_FAILURE;
    const auto display_time = frame_end_info ? frame_end_info->displayTime : 0;
    const auto result = fn(session, frame_end_info);
    const auto return_timestamp_ns = get_timestamp();
    if (xrabi::succeeded(result) && frame_end_info) {
        const auto& xr = Config::get().snapshot().openxr;
        if (xr.enabled && xr.timeline) {
            if (auto* ctx = LowLatencyCtx::get()) {
                const auto submitted_host_ns =
                    xr_time_to_host_ns(session, display_time, xr.clock_sync, ClockSampleKind::End);
                ctx->OpenXROnEndFrame(
                    reinterpret_cast<std::uintptr_t>(session),
                    display_time,
                    return_timestamp_ns,
                    submitted_host_ns);
            }
        }
    }
    return result;
}

xrabi::XrResult WINAPI OpenXRHooks::hkxrDestroySession(xrabi::XrSession session) {
    const auto fn = downstream(g_xr_destroy_session, g_dispatch_destroy_session);
    if (!fn) return xrabi::XR_ERROR_RUNTIME_FAILURE;
    const auto result = fn(session);
    if (xrabi::succeeded(result)) {
        clear_clock_binding(session);
        if (auto* ctx = LowLatencyCtx::get())
            ctx->OpenXROnDestroySession(reinterpret_cast<std::uintptr_t>(session));
    }
    return result;
}
