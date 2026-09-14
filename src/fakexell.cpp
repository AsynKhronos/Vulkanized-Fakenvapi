#include "fakexell.h"

#include "fakenvapi.h"
#include "log.h"

#include <detours.h>
#include <xell.h>
#include <xell_d3d12.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace fakexell {
namespace {

using PFN_xellDestroyContext = decltype(&xellDestroyContext);
using PFN_xellSetSleepMode = decltype(&xellSetSleepMode);
using PFN_xellGetSleepMode = decltype(&xellGetSleepMode);
using PFN_xellSleep = decltype(&xellSleep);
using PFN_xellAddMarkerData = decltype(&xellAddMarkerData);
using PFN_xellGetVersion = decltype(&xellGetVersion);
using PFN_xellSetLoggingCallback = decltype(&xellSetLoggingCallback);
using PFN_xellGetFramesReports = decltype(&xellGetFramesReports);
using PFN_xellD3D12CreateContext = decltype(&xellD3D12CreateContext);

PFN_xellDestroyContext o_xellDestroyContext = nullptr;
PFN_xellSetSleepMode o_xellSetSleepMode = nullptr;
PFN_xellGetSleepMode o_xellGetSleepMode = nullptr;
PFN_xellSleep o_xellSleep = nullptr;
PFN_xellAddMarkerData o_xellAddMarkerData = nullptr;
PFN_xellGetVersion o_xellGetVersion = nullptr;
PFN_xellSetLoggingCallback o_xellSetLoggingCallback = nullptr;
PFN_xellGetFramesReports o_xellGetFramesReports = nullptr;
PFN_xellD3D12CreateContext o_xellD3D12CreateContext = nullptr;

std::mutex hook_mutex;
std::atomic<bool> hooked{false};

struct SyntheticGameContext {
    std::atomic<ID3D12Device*> device{nullptr};
    std::atomic<std::uint64_t> sleep_mode{0};
    std::atomic<bool> active{false};
};

SyntheticGameContext game_context;

constexpr std::uint64_t kSleepValid = 1ull << 0;
constexpr std::uint64_t kSleepEnabled = 1ull << 1;
constexpr std::uint64_t kSleepBoost = 1ull << 2;

xell_context_handle_t synthetic_handle() noexcept {
    return reinterpret_cast<xell_context_handle_t>(&game_context);
}

std::uint64_t pack_xell_sleep(const xell_sleep_params_t& value) noexcept {
    std::uint64_t packed = kSleepValid;
    if (value.bLowLatencyMode) packed |= kSleepEnabled;
    if (value.bLowLatencyBoost) packed |= kSleepBoost;
    packed |= static_cast<std::uint64_t>(value.minimumIntervalUs) << 32;
    return packed;
}

void unpack_xell_sleep(std::uint64_t packed, xell_sleep_params_t* value) noexcept {
    if (!value) return;
    *value = {};
    value->minimumIntervalUs = static_cast<std::uint32_t>(packed >> 32);
    value->bLowLatencyMode = (packed & kSleepEnabled) != 0;
    value->bLowLatencyBoost = (packed & kSleepBoost) != 0;
}

HMODULE caller_module(void* return_address) noexcept {
    HMODULE module = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(return_address),
        &module);
    return module;
}

HMODULE this_module() noexcept {
    HMODULE module = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&this_module),
        &module);
    return module;
}

const char* module_basename(const char* path) noexcept {
    if (!path) return "";
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    return base;
}

bool same_directory(const char* lhs, const char* rhs) noexcept {
    if (!lhs || !rhs) return false;
    const char* lhs_base = module_basename(lhs);
    const char* rhs_base = module_basename(rhs);
    const auto lhs_len = static_cast<std::size_t>(lhs_base - lhs);
    const auto rhs_len = static_cast<std::size_t>(rhs_base - rhs);
    if (lhs_len != rhs_len) return false;
    return _strnicmp(lhs, rhs, lhs_len) == 0;
}

bool infrastructure_module(const char* base) noexcept {
    static constexpr const char* names[] = {
        "fakenvapi.dll", "nvapi.dll", "nvapi64.dll", "libxell.dll",
        "d3d12.dll", "dxgi.dll", "optiscaler.dll",
        "sl.interposer.dll", "sl.common.dll", "sl.reflex.dll", "sl.pcl.dll"
    };
    for (const char* name : names) {
        if (_stricmp(base, name) == 0) return true;
    }
    return false;
}

bool called_by_game_family(void* return_address, char* caller_name, std::size_t caller_name_size) noexcept {
    HMODULE caller = caller_module(return_address);
    if (!caller || caller == this_module()) return false;

    char caller_path[MAX_PATH]{};
    char exe_path[MAX_PATH]{};
    if (!GetModuleFileNameA(caller, caller_path, MAX_PATH) ||
        !GetModuleFileNameA(nullptr, exe_path, MAX_PATH)) {
        return false;
    }

    const char* base = module_basename(caller_path);
    if (caller_name && caller_name_size) {
        std::strncpy(caller_name, base, caller_name_size - 1);
        caller_name[caller_name_size - 1] = '\0';
    }

    // Nixxes and other engines commonly issue XeLL creation from a game DLL
    // rather than directly from the EXE. Treat ordinary modules beside the EXE
    // as game-owned, but never hijack our own/OptiScaler/Streamline plumbing.
    if (infrastructure_module(base)) return false;
    return caller == GetModuleHandleA(nullptr) || same_directory(caller_path, exe_path);
}

bool is_synthetic_handle(xell_context_handle_t context) noexcept {
    return context == synthetic_handle();
}

bool is_synthetic_context(xell_context_handle_t context) noexcept {
    return is_synthetic_handle(context) && game_context.active.load(std::memory_order_acquire);
}

ID3D12Device* acquire_game_device() noexcept {
    ID3D12Device* device = game_context.device.load(std::memory_order_acquire);
    if (device) device->AddRef();
    return device;
}

void replace_game_device(ID3D12Device* device) noexcept {
    if (device) device->AddRef();
    ID3D12Device* old = game_context.device.exchange(device, std::memory_order_acq_rel);
    if (old) old->Release();
}

void clear_game_context() noexcept {
    game_context.active.store(false, std::memory_order_release);
    game_context.sleep_mode.store(0, std::memory_order_release);
    replace_game_device(nullptr);
}

MarkerType normalize_xell_marker(xell_latency_marker_type_t marker) noexcept {
    switch (marker) {
        case XELL_SIMULATION_START: return MarkerType::SIMULATION_START;
        case XELL_SIMULATION_END: return MarkerType::SIMULATION_END;
        case XELL_RENDERSUBMIT_START: return MarkerType::RENDERSUBMIT_START;
        case XELL_RENDERSUBMIT_END: return MarkerType::RENDERSUBMIT_END;
        case XELL_PRESENT_START: return MarkerType::PRESENT_START;
        case XELL_PRESENT_END: return MarkerType::PRESENT_END;
        case XELL_INPUT_SAMPLE: return MarkerType::INPUT_SAMPLE;
        case XELL_MARKER_COUNT: break;
    }
    return MarkerType::SIMULATION_START;
}

xell_result_t hkxellDestroyContext(xell_context_handle_t context) {
    if (is_synthetic_handle(context)) {
        clear_game_context();
        return XELL_RESULT_SUCCESS;
    }
    return o_xellDestroyContext ? o_xellDestroyContext(context) : XELL_RESULT_ERROR_UNINITIALIZED;
}

xell_result_t hkxellSetSleepMode(xell_context_handle_t context, const xell_sleep_params_t* param) {
    if (is_synthetic_handle(context) && !is_synthetic_context(context)) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    if (!is_synthetic_context(context)) {
        return o_xellSetSleepMode ? o_xellSetSleepMode(context, param) : XELL_RESULT_ERROR_UNINITIALIZED;
    }
    if (!param) return XELL_RESULT_ERROR_INVALID_ARGUMENT;

    const auto previous_mode = game_context.sleep_mode.exchange(pack_xell_sleep(*param), std::memory_order_acq_rel);
    if ((previous_mode & kSleepValid) == 0) {
        spdlog::info("XeLL game input active: contributing mode/boost/interval and frame markers to aspect arbitration");
    }

    SleepMode mode{};
    mode.low_latency_enabled = param->bLowLatencyMode != 0;
    mode.low_latency_boost = param->bLowLatencyBoost != 0;
    mode.minimum_interval_us = param->minimumIntervalUs;
    mode.use_markers_to_optimize = true;

    ID3D12Device* device = acquire_game_device();
    const bool ok = LowLatencyCtx::get()->FrontendSetSleepMode(
        policy::InputFrontend::XeLL,
        policy::GraphicsApi::D3D12,
        device,
        mode);
    if (device) device->Release();
    return ok ? XELL_RESULT_SUCCESS : XELL_RESULT_ERROR_DEVICE;
}

xell_result_t hkxellGetSleepMode(xell_context_handle_t context, xell_sleep_params_t* param) {
    if (is_synthetic_handle(context) && !is_synthetic_context(context)) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    if (!is_synthetic_context(context)) {
        return o_xellGetSleepMode ? o_xellGetSleepMode(context, param) : XELL_RESULT_ERROR_UNINITIALIZED;
    }
    if (!param) return XELL_RESULT_ERROR_INVALID_ARGUMENT;

    unpack_xell_sleep(game_context.sleep_mode.load(std::memory_order_acquire), param);
    return XELL_RESULT_SUCCESS;
}

xell_result_t hkxellSleep(xell_context_handle_t context, std::uint32_t frame_id) {
    if (is_synthetic_handle(context) && !is_synthetic_context(context)) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    if (!is_synthetic_context(context)) {
        return o_xellSleep ? o_xellSleep(context, frame_id) : XELL_RESULT_ERROR_UNINITIALIZED;
    }

    ID3D12Device* device = acquire_game_device();
    const bool ok = LowLatencyCtx::get()->FrontendSleep(
        policy::InputFrontend::XeLL,
        policy::GraphicsApi::D3D12,
        device,
        frame_id);
    if (device) device->Release();
    return ok ? XELL_RESULT_SUCCESS : XELL_RESULT_ERROR_DEVICE;
}

xell_result_t hkxellAddMarkerData(
    xell_context_handle_t context,
    std::uint32_t frame_id,
    xell_latency_marker_type_t marker) {
    if (is_synthetic_handle(context) && !is_synthetic_context(context)) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    if (!is_synthetic_context(context)) {
        return o_xellAddMarkerData ? o_xellAddMarkerData(context, frame_id, marker)
                                   : XELL_RESULT_ERROR_UNINITIALIZED;
    }
    if (marker < XELL_SIMULATION_START || marker >= XELL_MARKER_COUNT) {
        return XELL_RESULT_ERROR_INVALID_ARGUMENT;
    }

    ID3D12Device* device = acquire_game_device();
    const bool ok = LowLatencyCtx::get()->FrontendSetMarker(
        policy::InputFrontend::XeLL,
        policy::GraphicsApi::D3D12,
        device,
        normalize_xell_marker(marker),
        frame_id);
    if (device) device->Release();
    return ok ? XELL_RESULT_SUCCESS : XELL_RESULT_ERROR_DEVICE;
}

xell_result_t hkxellGetVersion(xell_version_t* version) {
    // Version queries do not drive latency and are safe to forward.  This also
    // preserves the real SDK version even when the game context is synthetic.
    return o_xellGetVersion ? o_xellGetVersion(version) : XELL_RESULT_ERROR_UNINITIALIZED;
}

xell_result_t hkxellSetLoggingCallback(
    xell_context_handle_t context,
    xell_logging_level_t logging_level,
    xell_app_log_callback_t logging_callback) {
    if (is_synthetic_handle(context) && !is_synthetic_context(context)) return XELL_RESULT_ERROR_INVALID_CONTEXT;
    if (is_synthetic_context(context)) return XELL_RESULT_SUCCESS;
    return o_xellSetLoggingCallback
        ? o_xellSetLoggingCallback(context, logging_level, logging_callback)
        : XELL_RESULT_ERROR_UNINITIALIZED;
}

xell_result_t hkxellGetFramesReports(
    xell_context_handle_t context,
    xell_frame_report_t* out_data) {
    if (is_synthetic_handle(context) && !is_synthetic_context(context)) {
        return XELL_RESULT_ERROR_INVALID_CONTEXT;
    }
    if (!is_synthetic_context(context)) {
        return o_xellGetFramesReports
            ? o_xellGetFramesReports(context, out_data)
            : XELL_RESULT_ERROR_UNINITIALIZED;
    }
    if (!out_data) return XELL_RESULT_ERROR_INVALID_ARGUMENT;

    // XeLL specifies an array containing the last 64 frame reports.  We do not
    // fabricate timings: capability emulation only needs a valid, safe result
    // for a synthetic game context.
    std::memset(out_data, 0, sizeof(xell_frame_report_t) * 64);
    return XELL_RESULT_SUCCESS;
}

xell_result_t hkxellD3D12CreateContext(ID3D12Device* device, xell_context_handle_t* out_context) {
    char caller_name[MAX_PATH]{};
    if (!called_by_game_family(__builtin_return_address(0), caller_name, sizeof(caller_name))) {
        return o_xellD3D12CreateContext
            ? o_xellD3D12CreateContext(device, out_context)
            : XELL_RESULT_ERROR_UNINITIALIZED;
    }
    if (!device || !out_context) return XELL_RESULT_ERROR_INVALID_ARGUMENT;

    replace_game_device(device);
    game_context.sleep_mode.store(kSleepValid, std::memory_order_release);
    game_context.active.store(true, std::memory_order_release);
    *out_context = synthetic_handle();

    if (auto* runtime = LowLatencyCtx::get()) {
        runtime->FrontendObserveEvidence(
            policy::InputFrontend::XeLL, policy::InputEvidenceContext);
    }

    spdlog::info(
        "XeLL game-facing capability: synthetic D3D12 context created for {}",
        caller_name[0] ? caller_name : "game module");
    return XELL_RESULT_SUCCESS;
}

#define HOOK_XELL(name) \
    do { \
        if (o_##name) { \
            DetourAttach(reinterpret_cast<PVOID*>(&o_##name), reinterpret_cast<PVOID>(hk##name)); \
        } \
    } while (false)

} // namespace

bool Init() {
    if (hooked.load(std::memory_order_acquire)) return true;

    std::scoped_lock lock(hook_mutex);
    if (hooked.load(std::memory_order_relaxed)) return true;

    HMODULE xell_module = GetModuleHandleA("libxell.dll");
    if (!xell_module) return false;

    o_xellDestroyContext = reinterpret_cast<PFN_xellDestroyContext>(GetProcAddress(xell_module, "xellDestroyContext"));
    o_xellSetSleepMode = reinterpret_cast<PFN_xellSetSleepMode>(GetProcAddress(xell_module, "xellSetSleepMode"));
    o_xellGetSleepMode = reinterpret_cast<PFN_xellGetSleepMode>(GetProcAddress(xell_module, "xellGetSleepMode"));
    o_xellSleep = reinterpret_cast<PFN_xellSleep>(GetProcAddress(xell_module, "xellSleep"));
    o_xellAddMarkerData = reinterpret_cast<PFN_xellAddMarkerData>(GetProcAddress(xell_module, "xellAddMarkerData"));
    o_xellGetVersion = reinterpret_cast<PFN_xellGetVersion>(GetProcAddress(xell_module, "xellGetVersion"));
    o_xellSetLoggingCallback = reinterpret_cast<PFN_xellSetLoggingCallback>(GetProcAddress(xell_module, "xellSetLoggingCallback"));
    o_xellGetFramesReports = reinterpret_cast<PFN_xellGetFramesReports>(GetProcAddress(xell_module, "xellGetFramesReports"));
    o_xellD3D12CreateContext = reinterpret_cast<PFN_xellD3D12CreateContext>(GetProcAddress(xell_module, "xellD3D12CreateContext"));

    if (!o_xellD3D12CreateContext || !o_xellAddMarkerData || !o_xellSleep || !o_xellSetSleepMode) {
        spdlog::warn("XeLL input frontend: required exports missing");
        return false;
    }

    if (DetourTransactionBegin() != NO_ERROR) return false;
    DetourUpdateThread(GetCurrentThread());

    HOOK_XELL(xellDestroyContext);
    HOOK_XELL(xellSetSleepMode);
    HOOK_XELL(xellGetSleepMode);
    HOOK_XELL(xellSleep);
    HOOK_XELL(xellAddMarkerData);
    HOOK_XELL(xellGetVersion);
    HOOK_XELL(xellSetLoggingCallback);
    HOOK_XELL(xellGetFramesReports);
    HOOK_XELL(xellD3D12CreateContext);

    const LONG result = DetourTransactionCommit();
    if (result != NO_ERROR) {
        spdlog::error("XeLL input frontend: Detour transaction failed ({})", result);
        return false;
    }

    hooked.store(true, std::memory_order_release);
    spdlog::info("XeLL input frontend hooked");
    return true;
}

bool IsHooked() noexcept {
    return hooked.load(std::memory_order_acquire);
}

} // namespace fakexell
