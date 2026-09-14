#include "streamline_capability.h"

#include "log.h"

#include <detours.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace StreamlineCapability {
namespace {

// Minimal Streamline ABI declarations. These mirror NVIDIA Streamline's public
// headers without taking a source/build dependency on Streamline itself.
namespace slabi {

using Feature = std::uint32_t;
constexpr Feature kFeatureReflex = 3;

enum class Result : int {
    eOk = 0,
    eErrorIO,
    eErrorDriverOutOfDate,
    eErrorOSOutOfDate,
    eErrorOSDisabledHWS,
    eErrorDeviceNotCreated,
    eErrorNoSupportedAdapterFound,
    eErrorAdapterNotSupported,
    eErrorNoPlugins,
    eErrorVulkanAPI,
    eErrorDXGIAPI,
    eErrorD3DAPI,
    eErrorNRDAPI,
    eErrorNVAPI,
    eErrorReflexAPI,
    eErrorNGXFailed,
    eErrorJSONParsing,
    eErrorMissingProxy,
    eErrorMissingResourceState,
    eErrorInvalidIntegration,
    eErrorMissingInputParameter,
    eErrorNotInitialized,
    eErrorComputeFailed,
    eErrorInitNotCalled,
    eErrorExceptionHandler,
    eErrorInvalidParameter,
    eErrorMissingConstants,
    eErrorDuplicatedConstants,
    eErrorMissingOrInvalidAPI,
    eErrorCommonConstantsMissing,
    eErrorUnsupportedInterface,
    eErrorFeatureMissing,
    eErrorFeatureNotSupported,
};

struct AdapterInfo;

struct StructType {
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::uint8_t data4[8];
};

struct BaseStructure {
    BaseStructure* next;
    StructType structType;
    std::size_t structVersion;
};

// Flatten the public BaseStructure prefix instead of inheriting from it.  This
// keeps offsetof well-defined while preserving the exact Streamline ABI layout
// for the first ReflexState fields that we are allowed to touch.
struct ReflexStatePrefix {
    BaseStructure* next;
    StructType structType;
    std::size_t structVersion;
    bool lowLatencyAvailable;
    bool latencyReportAvailable;
};

using PFun_slIsFeatureSupported = Result(Feature feature, const AdapterInfo& adapterInfo);
using PFun_slGetFeatureFunction = Result(Feature feature, const char* functionName, void*& function);
using PFun_slReflexGetState = Result(ReflexStatePrefix& state);

static_assert(sizeof(StructType) == 16);
#if INTPTR_MAX == INT64_MAX
static_assert(offsetof(ReflexStatePrefix, lowLatencyAvailable) == 32);
#else
static_assert(offsetof(ReflexStatePrefix, lowLatencyAvailable) == 24);
#endif

} // namespace slabi

slabi::PFun_slIsFeatureSupported* o_slIsFeatureSupported = nullptr;
slabi::PFun_slGetFeatureFunction* o_slGetFeatureFunction = nullptr;
std::atomic<slabi::PFun_slReflexGetState*> o_slReflexGetState{nullptr};

std::mutex hook_mutex;
std::atomic<bool> hooked{false};
std::atomic<bool> logged_support_override{false};
std::atomic<bool> logged_state_override{false};

slabi::Result hk_slReflexGetState(slabi::ReflexStatePrefix& state) {
    auto* original = o_slReflexGetState.load(std::memory_order_acquire);
    const auto result = original ? original(state) : slabi::Result::eErrorReflexAPI;

    // Reflex Low Latency UI availability is a capability result, not the
    // selected output technology. Vulkanized-Fakenvapi can translate the
    // resulting game-facing Reflex frontend to XeLL/AL2/LatencyFlex.
    state.lowLatencyAvailable = true;

    if (!logged_state_override.exchange(true, std::memory_order_acq_rel)) {
        spdlog::info(
            "Streamline capability: ReflexState.lowLatencyAvailable forced true (native result {})",
            static_cast<int>(result));
    }

    // If Streamline returned a valid state, preserve success. If the plugin is
    // present but reports a hardware-only Reflex failure, expose the emulated
    // capability to the host so it can enable its Reflex UI and call path.
    return slabi::Result::eOk;
}

slabi::Result hk_slIsFeatureSupported(
    slabi::Feature feature,
    const slabi::AdapterInfo& adapterInfo) {
    if (feature != slabi::kFeatureReflex) {
        return o_slIsFeatureSupported
            ? o_slIsFeatureSupported(feature, adapterInfo)
            : slabi::Result::eErrorFeatureMissing;
    }

    const auto native_result = o_slIsFeatureSupported
        ? o_slIsFeatureSupported(feature, adapterInfo)
        : slabi::Result::eErrorFeatureMissing;

    if (!logged_support_override.exchange(true, std::memory_order_acq_rel)) {
        spdlog::info(
            "Streamline capability: slIsFeatureSupported(Reflex) -> supported (native result {})",
            static_cast<int>(native_result));
    }

    return slabi::Result::eOk;
}

slabi::Result hk_slGetFeatureFunction(
    slabi::Feature feature,
    const char* function_name,
    void*& function) {
    const auto result = o_slGetFeatureFunction
        ? o_slGetFeatureFunction(feature, function_name, function)
        : slabi::Result::eErrorFeatureMissing;

    if (feature != slabi::kFeatureReflex || !function_name) return result;

    if (std::strcmp(function_name, "slReflexGetState") == 0) {
        if (result == slabi::Result::eOk && function) {
            auto* original = reinterpret_cast<slabi::PFun_slReflexGetState*>(function);
            auto* expected = static_cast<slabi::PFun_slReflexGetState*>(nullptr);
            o_slReflexGetState.compare_exchange_strong(
                expected,
                original,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
        }

        // Return the capability wrapper even when the native plugin reports a
        // hardware-only state failure. This is deliberately limited to the
        // state query; the actual Reflex execution functions still come from
        // Streamline and ultimately feed fakenvapi/NVAPI as before.
        function = reinterpret_cast<void*>(&hk_slReflexGetState);
        return slabi::Result::eOk;
    }

    return result;
}

HMODULE find_streamline_core() noexcept {
    // The game imports Streamline core APIs from sl.interposer in normal
    // interposer integrations. sl.common is a fallback for unusual layouts.
    if (auto* module = GetModuleHandleA("sl.interposer.dll")) return module;
    return GetModuleHandleA("sl.common.dll");
}

} // namespace

bool Init() {
    if (hooked.load(std::memory_order_acquire)) return true;

    std::scoped_lock lock(hook_mutex);
    if (hooked.load(std::memory_order_relaxed)) return true;

    HMODULE module = find_streamline_core();
    if (!module) return false;

    o_slIsFeatureSupported = reinterpret_cast<slabi::PFun_slIsFeatureSupported*>(
        GetProcAddress(module, "slIsFeatureSupported"));
    o_slGetFeatureFunction = reinterpret_cast<slabi::PFun_slGetFeatureFunction*>(
        GetProcAddress(module, "slGetFeatureFunction"));

    if (!o_slIsFeatureSupported || !o_slGetFeatureFunction) {
        spdlog::warn("Streamline capability: required core exports missing");
        o_slIsFeatureSupported = nullptr;
        o_slGetFeatureFunction = nullptr;
        return false;
    }

    if (DetourTransactionBegin() != NO_ERROR) return false;
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(
        reinterpret_cast<PVOID*>(&o_slIsFeatureSupported),
        reinterpret_cast<PVOID>(hk_slIsFeatureSupported));
    DetourAttach(
        reinterpret_cast<PVOID*>(&o_slGetFeatureFunction),
        reinterpret_cast<PVOID>(hk_slGetFeatureFunction));

    const LONG result = DetourTransactionCommit();
    if (result != NO_ERROR) {
        spdlog::error("Streamline capability: Detour transaction failed ({})", result);
        o_slIsFeatureSupported = nullptr;
        o_slGetFeatureFunction = nullptr;
        return false;
    }

    hooked.store(true, std::memory_order_release);
    spdlog::info("Streamline Reflex game-facing capability emulation hooked");
    return true;
}

bool IsHooked() noexcept {
    return hooked.load(std::memory_order_acquire);
}

} // namespace StreamlineCapability
