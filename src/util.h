#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "nvapi_mingw_compat.h"
#include "clock_math.h"
#include "wait_policy.h"
#include <string>
#include <algorithm>
#include <atomic>
#include <map>
#include <cstdint>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

void tonvss(NvAPI_ShortString nvss, std::string str);

#define INSERT_AND_RETURN_WHEN_EQUALS(method) \
    if (std::string(it->func) == #method)     \
        return registry.insert({id, (void *)method}).first->second;

// R4.2 canonical host clock. QueryPerformanceCounter is monotonic and is also
// the clock domain used by XR_KHR_win32_convert_performance_counter_time.
// Keeping graphics/XR timing in this domain avoids wall-clock steps and avoids
// assuming a cached offset between XrTime and a system clock.
static inline uint64_t qpc_frequency() noexcept {
    static const uint64_t frequency = []() noexcept {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0
            ? static_cast<uint64_t>(value.QuadPart)
            : 0ull;
    }();
    return frequency;
}

static inline uint64_t qpc_ticks_to_ns(int64_t ticks) noexcept {
    if (ticks < 0) return 0;
    return hostclock::ticks_to_ns(static_cast<uint64_t>(ticks), qpc_frequency());
}

static inline uint64_t get_timestamp() noexcept {
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter)) return 0;
    return qpc_ticks_to_ns(counter.QuadPart);
}

// High-resolution wait implementation used by LatencyFleX/VulkanFlex.
//
// Design goals:
// - no shared timer object between pacing threads;
// - high-resolution timer when available, ordinary waitable timer otherwise;
// - no silent "did not wait" failure when the kernel timer path is unavailable;
// - a short adaptive spin tail instead of the historical fixed 2 ms busy-wait;
// - PAUSE in the spin loop so the pacing thread does not monopolize an SMT core.
namespace waitdetail {

struct ThreadWaitableTimer {
    HANDLE handle = nullptr;

    ThreadWaitableTimer() noexcept {
        handle = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!handle) {
            // Wine/older Windows builds may reject HIGH_RESOLUTION. A normal
            // waitable timer is still preferable to dropping the pacing wait.
            handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        if (!handle)
            handle = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    ~ThreadWaitableTimer() {
        if (handle)
            CloseHandle(handle);
    }

    ThreadWaitableTimer(const ThreadWaitableTimer&) = delete;
    ThreadWaitableTimer& operator=(const ThreadWaitableTimer&) = delete;
};

inline HANDLE thread_timer() noexcept {
    static thread_local ThreadWaitableTimer timer;
    return timer.handle;
}

inline void cpu_relax() noexcept {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#  if defined(_MSC_VER)
    _mm_pause();
#  else
    __builtin_ia32_pause();
#  endif
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

inline int waitable_timer_sleep_ns(std::int64_t ns) noexcept {
    if (ns <= 0)
        return 0;

    HANDLE timer = thread_timer();
    if (!timer)
        return 1;

    // Windows relative due times use 100 ns units and must be negative. Keep a
    // minimum of one tick so tiny coarse portions never become an absolute zero.
    LARGE_INTEGER due_time{};
    const auto ticks_100ns = std::max<std::int64_t>(1, ns / 100);
    due_time.QuadPart = -ticks_100ns;

    if (!SetWaitableTimerEx(timer, &due_time, 0, nullptr, nullptr, nullptr, 0)) {
        // Some compatibility layers support the ordinary API but not Ex.
        if (!SetWaitableTimer(timer, &due_time, 0, nullptr, nullptr, FALSE))
            return 2;
    }

    return WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0 ? 0 : 3;
}

inline int spin_until(std::uint64_t deadline_ns) noexcept {
    if (deadline_ns == 0)
        return 4;

    for (;;) {
        const auto now = get_timestamp();
        if (now == 0)
            return 4;
        if (now >= deadline_ns)
            return 0;
        cpu_relax();
    }
}

} // namespace waitdetail

inline int timer_sleep(int64_t hundred_ns) {
    if (hundred_ns <= 0)
        return 0;
    return waitdetail::waitable_timer_sleep_ns(hundred_ns * 100);
}

inline int busywait_sleep(int64_t ns) {
    if (ns <= 0)
        return 0;
    const auto start = get_timestamp();
    if (start == 0)
        return 4;
    const auto delta = static_cast<std::uint64_t>(ns);
    const auto deadline = start > UINT64_MAX - delta ? UINT64_MAX : start + delta;
    return waitdetail::spin_until(deadline);
}

inline int eepy(int64_t ns) {
    if (ns <= 0)
        return 0;

    const auto start = get_timestamp();
    if (start == 0)
        return 4;

    const auto delta = static_cast<std::uint64_t>(ns);
    const auto deadline = start > UINT64_MAX - delta ? UINT64_MAX : start + delta;
    const auto plan = waitpolicy::make_wait_plan(ns);

    if (plan.timer_ns > 0) {
        const int timer_status = waitdetail::waitable_timer_sleep_ns(plan.timer_ns);
        if (timer_status != 0) {
            // Correctness beats power efficiency: if a compatibility layer has
            // no usable timer primitive, preserve pacing by falling back to the
            // bounded spin-to-deadline rather than silently returning early.
            return waitdetail::spin_until(deadline);
        }
    }

    // Always close on the absolute deadline. This automatically corrects an
    // early timer wake and returns immediately after an unavoidable oversleep.
    return waitdetail::spin_until(deadline);
}

// function taken from jp7677's dxvk-nvapi project licensed under MIT
inline std::string from_error_nr(const int16_t error_nr) {
    static const std::map<int16_t, std::string> errors{
        {-1, "NVAPI_ERROR"},
        {-2, "NVAPI_LIBRARY_NOT_FOUND"},
        {-3, "NVAPI_NO_IMPLEMENTATION"},
        {-4, "NVAPI_API_NOT_INITIALIZED"},
        {-5, "NVAPI_INVALID_ARGUMENT"},
        {-6, "NVAPI_NVIDIA_DEVICE_NOT_FOUND"},
        {-7, "NVAPI_END_ENUMERATION"},
        {-8, "NVAPI_INVALID_HANDLE"},
        {-9, "NVAPI_INCOMPATIBLE_STRUCT_VERSION"},
        {-10, "NVAPI_HANDLE_INVALIDATED"},
        {-11, "NVAPI_OPENGL_CONTEXT_NOT_CURRENT"},
        {-14, "NVAPI_INVALID_POINTER"},
        {-12, "NVAPI_NO_GL_EXPERT"},
        {-13, "NVAPI_INSTRUMENTATION_DISABLED"},
        {-15, "NVAPI_NO_GL_NSIGHT"},
        {-100, "NVAPI_EXPECTED_LOGICAL_GPU_HANDLE"},
        {-101, "NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE"},
        {-102, "NVAPI_EXPECTED_DISPLAY_HANDLE"},
        {-103, "NVAPI_INVALID_COMBINATION"},
        {-104, "NVAPI_NOT_SUPPORTED"},
        {-105, "NVAPI_PORTID_NOT_FOUND"},
        {-106, "NVAPI_EXPECTED_UNATTACHED_DISPLAY_HANDLE"},
        {-107, "NVAPI_INVALID_PERF_LEVEL"},
        {-108, "NVAPI_DEVICE_BUSY"},
        {-109, "NVAPI_NV_PERSIST_FILE_NOT_FOUND"},
        {-110, "NVAPI_PERSIST_DATA_NOT_FOUND"},
        {-111, "NVAPI_EXPECTED_TV_DISPLAY"},
        {-112, "NVAPI_EXPECTED_TV_DISPLAY_ON_DCONNECTOR"},
        {-113, "NVAPI_NO_ACTIVE_SLI_TOPOLOGY"},
        {-114, "NVAPI_SLI_RENDERING_MODE_NOTALLOWED"},
        {-115, "NVAPI_EXPECTED_DIGITAL_FLAT_PANEL"},
        {-116, "NVAPI_ARGUMENT_EXCEED_MAX_SIZE"},
        {-117, "NVAPI_DEVICE_SWITCHING_NOT_ALLOWED"},
        {-118, "NVAPI_TESTING_CLOCKS_NOT_SUPPORTED"},
        {-119, "NVAPI_UNKNOWN_UNDERSCAN_CONFIG"},
        {-120, "NVAPI_TIMEOUT_RECONFIGURING_GPU_TOPO"},
        {-121, "NVAPI_DATA_NOT_FOUND"},
        {-122, "NVAPI_EXPECTED_ANALOG_DISPLAY"},
        {-123, "NVAPI_NO_VIDLINK"},
        {-124, "NVAPI_REQUIRES_REBOOT"},
        {-125, "NVAPI_INVALID_HYBRID_MODE"},
        {-126, "NVAPI_MIXED_TARGET_TYPES"},
        {-127, "NVAPI_SYSWOW64_NOT_SUPPORTED"},
        {-128, "NVAPI_IMPLICIT_SET_GPU_TOPOLOGY_CHANGE_NOT_ALLOWED"},
        {-129, "NVAPI_REQUEST_USER_TO_CLOSE_NON_MIGRATABLE_APPS"},
        {-130, "NVAPI_OUT_OF_MEMORY"},
        {-131, "NVAPI_WAS_STILL_DRAWING"},
        {-132, "NVAPI_FILE_NOT_FOUND"},
        {-133, "NVAPI_TOO_MANY_UNIQUE_STATE_OBJECTS"},
        {-134, "NVAPI_INVALID_CALL"},
        {-135, "NVAPI_D3D10_1_LIBRARY_NOT_FOUND"},
        {-136, "NVAPI_FUNCTION_NOT_FOUND"},
        {-137, "NVAPI_INVALID_USER_PRIVILEGE"},
        {-138, "NVAPI_EXPECTED_NON_PRIMARY_DISPLAY_HANDLE"},
        {-139, "NVAPI_EXPECTED_COMPUTE_GPU_HANDLE"},
        {-140, "NVAPI_STEREO_NOT_INITIALIZED"},
        {-141, "NVAPI_STEREO_REGISTRY_ACCESS_FAILED"},
        {-142, "NVAPI_STEREO_REGISTRY_PROFILE_TYPE_NOT_SUPPORTED"},
        {-143, "NVAPI_STEREO_REGISTRY_VALUE_NOT_SUPPORTED"},
        {-144, "NVAPI_STEREO_NOT_ENABLED"},
        {-145, "NVAPI_STEREO_NOT_TURNED_ON"},
        {-146, "NVAPI_STEREO_INVALID_DEVICE_INTERFACE"},
        {-147, "NVAPI_STEREO_PARAMETER_OUT_OF_RANGE"},
        {-148, "NVAPI_STEREO_FRUSTUM_ADJUST_MODE_NOT_SUPPORTED"},
        {-149, "NVAPI_TOPO_NOT_POSSIBLE"},
        {-150, "NVAPI_MODE_CHANGE_FAILED"},
        {-151, "NVAPI_D3D11_LIBRARY_NOT_FOUND"},
        {-152, "NVAPI_INVALID_ADDRESS"},
        {-153, "NVAPI_STRING_TOO_SMALL"},
        {-154, "NVAPI_MATCHING_DEVICE_NOT_FOUND"},
        {-155, "NVAPI_DRIVER_RUNNING"},
        {-156, "NVAPI_DRIVER_NOTRUNNING"},
        {-157, "NVAPI_ERROR_DRIVER_RELOAD_REQUIRED"},
        {-158, "NVAPI_SET_NOT_ALLOWED"},
        {-159, "NVAPI_ADVANCED_DISPLAY_TOPOLOGY_REQUIRED"},
        {-160, "NVAPI_SETTING_NOT_FOUND"},
        {-161, "NVAPI_SETTING_SIZE_TOO_LARGE"},
        {-162, "NVAPI_TOO_MANY_SETTINGS_IN_PROFILE"},
        {-163, "NVAPI_PROFILE_NOT_FOUND"},
        {-164, "NVAPI_PROFILE_NAME_IN_USE"},
        {-165, "NVAPI_PROFILE_NAME_EMPTY"},
        {-166, "NVAPI_EXECUTABLE_NOT_FOUND"},
        {-167, "NVAPI_EXECUTABLE_ALREADY_IN_USE"},
        {-168, "NVAPI_DATATYPE_MISMATCH"},
        {-169, "NVAPI_PROFILE_REMOVED"},
        {-170, "NVAPI_UNREGISTERED_RESOURCE"},
        {-171, "NVAPI_ID_OUT_OF_RANGE"},
        {-172, "NVAPI_DISPLAYCONFIG_VALIDATION_FAILED"},
        {-173, "NVAPI_DPMST_CHANGED"},
        {-174, "NVAPI_INSUFFICIENT_BUFFER"},
        {-175, "NVAPI_ACCESS_DENIED"},
        {-176, "NVAPI_MOSAIC_NOT_ACTIVE"},
        {-177, "NVAPI_SHARE_RESOURCE_RELOCATED"},
        {-178, "NVAPI_REQUEST_USER_TO_DISABLE_DWM"},
        {-179, "NVAPI_D3D_DEVICE_LOST"},
        {-180, "NVAPI_INVALID_CONFIGURATION"},
        {-181, "NVAPI_STEREO_HANDSHAKE_NOT_DONE"},
        {-182, "NVAPI_EXECUTABLE_PATH_IS_AMBIGUOUS"},
        {-183, "NVAPI_DEFAULT_STEREO_PROFILE_IS_NOT_DEFINED"},
        {-184, "NVAPI_DEFAULT_STEREO_PROFILE_DOES_NOT_EXIST"},
        {-185, "NVAPI_CLUSTER_ALREADY_EXISTS"},
        {-186, "NVAPI_DPMST_DISPLAY_ID_EXPECTED"},
        {-187, "NVAPI_INVALID_DISPLAY_ID"},
        {-188, "NVAPI_STREAM_IS_OUT_OF_SYNC"},
        {-189, "NVAPI_INCOMPATIBLE_AUDIO_DRIVER"},
        {-190, "NVAPI_VALUE_ALREADY_SET"},
        {-191, "NVAPI_TIMEOUT"},
        {-192, "NVAPI_GPU_WORKSTATION_FEATURE_INCOMPLETE"},
        {-193, "NVAPI_STEREO_INIT_ACTIVATION_NOT_DONE"},
        {-194, "NVAPI_SYNC_NOT_ACTIVE"},
        {-195, "NVAPI_SYNC_MASTER_NOT_FOUND"},
        {-196, "NVAPI_INVALID_SYNC_TOPOLOGY"},
        {-197, "NVAPI_ECID_SIGN_ALGO_UNSUPPORTED"},
        {-198, "NVAPI_ECID_KEY_VERIFICATION_FAILED"},
        {-199, "NVAPI_FIRMWARE_OUT_OF_DATE"},
        {-200, "NVAPI_FIRMWARE_REVISION_NOT_SUPPORTED"},
        {-201, "NVAPI_LICENSE_CALLER_AUTHENTICATION_FAILED"},
        {-202, "NVAPI_D3D_DEVICE_NOT_REGISTERED"},
        {-203, "NVAPI_RESOURCE_NOT_ACQUIRED"},
        {-204, "NVAPI_TIMING_NOT_SUPPORTED"},
        {-205, "NVAPI_HDCP_ENCRYPTION_FAILED"},
        {-206, "NVAPI_PCLK_LIMITATION_FAILED"},
        {-207, "NVAPI_NO_CONNECTOR_FOUND"},
        {-208, "NVAPI_HDCP_DISABLED"},
        {-209, "NVAPI_API_IN_USE"},
        {-210, "NVAPI_NVIDIA_DISPLAY_NOT_FOUND"},
        {-211, "NVAPI_PRIV_SEC_VIOLATION"},
        {-212, "NVAPI_INCORRECT_VENDOR"},
        {-213, "NVAPI_DISPLAY_IN_USE"},
        {-214, "NVAPI_UNSUPPORTED_CONFIG_NON_HDCP_HMD"},
        {-215, "NVAPI_MAX_DISPLAY_LIMIT_REACHED"},
        {-216, "NVAPI_INVALID_DIRECT_MODE_DISPLAY"},
        {-217, "NVAPI_GPU_IN_DEBUG_MODE"},
        {-218, "NVAPI_D3D_CONTEXT_NOT_FOUND"},
        {-219, "NVAPI_STEREO_VERSION_MISMATCH"},
        {-220, "NVAPI_GPU_NOT_POWERED"},
        {-221, "NVAPI_ERROR_DRIVER_RELOAD_IN_PROGRESS"},
        {-222, "NVAPI_WAIT_FOR_HW_RESOURCE"},
        {-223, "NVAPI_REQUIRE_FURTHER_HDCP_ACTION"},
        {-224, "NVAPI_DISPLAY_MUX_TRANSITION_FAILED"} };

    auto it = errors.find(error_nr);
    return it != errors.end() ? it->second : "UNKNOWN_ERROR";
}
