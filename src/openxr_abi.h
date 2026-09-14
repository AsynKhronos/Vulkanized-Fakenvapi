#pragma once

// Minimal OpenXR 1.0 core ABI used by the observer hooks.  The project does
// not link against the OpenXR loader and deliberately avoids vendoring the
// full SDK just to observe the frame loop.  Layouts/signatures below mirror
// the Khronos OpenXR core ABI for xrGetInstanceProcAddr/xrWaitFrame/
// xrBeginFrame/xrEndFrame/xrDestroySession.

#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xrabi {

using XrBool32 = std::uint32_t;
using XrFlags64 = std::uint64_t;
using XrTime = std::int64_t;
using XrDuration = std::int64_t;
using XrResult = std::int32_t;
using XrStructureType = std::int32_t;
using XrEnvironmentBlendMode = std::int32_t;

struct XrInstance_T;
struct XrSession_T;
struct XrSessionCreateInfo;
struct XrCompositionLayerBaseHeader;

using XrInstance = XrInstance_T*;
using XrSession = XrSession_T*;

constexpr XrResult XR_SUCCESS = 0;
constexpr XrResult XR_FRAME_DISCARDED = 9;
constexpr XrResult XR_ERROR_RUNTIME_FAILURE = -2;

[[nodiscard]] constexpr bool succeeded(XrResult result) noexcept {
    return result >= 0;
}

struct XrFrameWaitInfo {
    XrStructureType type;
    const void* next;
};

struct XrFrameState {
    XrStructureType type;
    void* next;
    XrTime predictedDisplayTime;
    XrDuration predictedDisplayPeriod;
    XrBool32 shouldRender;
};

struct XrFrameBeginInfo {
    XrStructureType type;
    const void* next;
};

struct XrFrameEndInfo {
    XrStructureType type;
    const void* next;
    XrTime displayTime;
    XrEnvironmentBlendMode environmentBlendMode;
    std::uint32_t layerCount;
    const XrCompositionLayerBaseHeader* const* layers;
};

using PFN_xrVoidFunction = void (WINAPI*)();
using PFN_xrGetInstanceProcAddr = XrResult (WINAPI*)(
    XrInstance instance, const char* name, PFN_xrVoidFunction* function);
using PFN_xrCreateSession = XrResult (WINAPI*)(
    XrInstance instance, const XrSessionCreateInfo* create_info, XrSession* session);
using PFN_xrWaitFrame = XrResult (WINAPI*)(
    XrSession session, const XrFrameWaitInfo* frame_wait_info, XrFrameState* frame_state);
using PFN_xrBeginFrame = XrResult (WINAPI*)(
    XrSession session, const XrFrameBeginInfo* frame_begin_info);
using PFN_xrEndFrame = XrResult (WINAPI*)(
    XrSession session, const XrFrameEndInfo* frame_end_info);
using PFN_xrDestroySession = XrResult (WINAPI*)(XrSession session);

// XR_KHR_win32_convert_performance_counter_time. These functions are resolved
// per XrInstance through xrGetInstanceProcAddr; they are never assumed enabled.
using PFN_xrConvertTimeToWin32PerformanceCounterKHR = XrResult (WINAPI*)(
    XrInstance instance, XrTime time, LARGE_INTEGER* performance_counter);

} // namespace xrabi
