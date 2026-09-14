#include "ll_xell.h"

#include "spoof.h"
#include "../fakexell.h"
#include <magic_enum.hpp>

bool XeLL::load_dll() {
    if (!xell_dll)
        xell_dll = LoadLibraryA("libxell.dll");

    if (!xell_dll) return false;

    // Loading the output backend may be the first time libxell.dll appears in
    // the process. Install the game-facing frontend detours now; calls made
    // from this DLL are detected as backend calls and forwarded to the real
    // XeLL implementation.
    fakexell::Init();

    o_xellD3D12CreateContext = (decltype(&xellD3D12CreateContext)) GetProcAddress(xell_dll, "xellD3D12CreateContext");
    o_xellDestroyContext = (decltype(&xellDestroyContext)) GetProcAddress(xell_dll, "xellDestroyContext");
    o_xellSetSleepMode = (decltype(&xellSetSleepMode)) GetProcAddress(xell_dll, "xellSetSleepMode");
    o_xellGetSleepMode = (decltype(&xellGetSleepMode)) GetProcAddress(xell_dll, "xellGetSleepMode");
    o_xellSleep = (decltype(&xellSleep)) GetProcAddress(xell_dll, "xellSleep");
    o_xellAddMarkerData = (decltype(&xellAddMarkerData)) GetProcAddress(xell_dll, "xellAddMarkerData");
    o_xellGetVersion = (decltype(&xellGetVersion)) GetProcAddress(xell_dll, "xellGetVersion");
    o_xellSetLoggingCallback = (decltype(&xellSetLoggingCallback)) GetProcAddress(xell_dll, "xellSetLoggingCallback");
    o_xellGetFramesReports = (decltype(&xellGetFramesReports)) GetProcAddress(xell_dll, "xellGetFramesReports");

    if (o_xellD3D12CreateContext && o_xellDestroyContext && o_xellSetSleepMode && o_xellGetSleepMode && o_xellSleep && o_xellAddMarkerData && o_xellGetVersion && o_xellSetLoggingCallback && o_xellGetFramesReports)
        return true;

    spdlog::info("Couldn't load complete libxell.dll API surface");
    unload_dll();
    return false;
}

bool XeLL::unload_dll() {
    if (xell_dll) {
        FreeLibrary(xell_dll);
        xell_dll = nullptr;
    }

    o_xellD3D12CreateContext = nullptr;
    o_xellDestroyContext = nullptr;
    o_xellSetSleepMode = nullptr;
    o_xellGetSleepMode = nullptr;
    o_xellSleep = nullptr;
    o_xellAddMarkerData = nullptr;
    o_xellGetVersion = nullptr;
    o_xellSetLoggingCallback = nullptr;
    o_xellGetFramesReports = nullptr;

    return !xell_dll;
}

void XeLL::xell_sleep(uint32_t frame_id) {
    sent_sleep_frame_ids_[frame_id % sent_sleep_frame_ids_.size()].store(
        frame_id, std::memory_order_release);
    o_xellSleep(ctx, frame_id);
}

void XeLL::add_marker(uint32_t frame_id, xell_latency_marker_type_t marker) {
    if (sent_sleep_frame_ids_[frame_id % sent_sleep_frame_ids_.size()].load(
            std::memory_order_acquire) != frame_id) {
        VFN_HOT_TRACE("Skipping reporting {} for XeLL because sleep wasn't sent for frame id: {}", magic_enum::enum_name(marker), frame_id);
        return;
    }

    o_xellAddMarkerData(ctx, frame_id, marker);
}

bool XeLL::init(IUnknown *pDevice) {
    if (!load_dll() || !pDevice || ctx)
        return false;

    ID3D12Device* dx12_pDevice = nullptr;
    HRESULT hr = pDevice->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dx12_pDevice));
    if (hr != S_OK)
        return false;

    hook_GetDesc1();

    spoof(true);
    auto result = o_xellD3D12CreateContext(dx12_pDevice, &ctx);
    spoof(false);
    dx12_pDevice->Release();

    unhook_GetDesc1();

    if (result == XELL_RESULT_SUCCESS && ctx) {
        last_sleep_mode_valid_ = false;
        for (auto& id : sent_sleep_frame_ids_) id.store(0, std::memory_order_relaxed);
        o_xellSetLoggingCallback(ctx, XELL_LOGGING_LEVEL_DEBUG, [](const char* message, xell_logging_level_t loggingLevel) {
            switch (loggingLevel) {
                case XELL_LOGGING_LEVEL_DEBUG:
                    spdlog::debug("XeLL: {}", message);
                break;
                case XELL_LOGGING_LEVEL_INFO:
                    spdlog::info("XeLL: {}", message);
                break;
                case XELL_LOGGING_LEVEL_WARNING:
                    spdlog::warn("XeLL: {}", message);
                break;
                case XELL_LOGGING_LEVEL_ERROR:
                    spdlog::error("XeLL: {}", message);
                break;
            }
        });
        spdlog::info("XeLL initialized");
    } else {
        spdlog::info("XeLL initialization failed: {}", (int32_t) result);
        deinit();
    }

    return result == XELL_RESULT_SUCCESS;
}

bool XeLL::init_using_ctx(void* context) {
    // XeLL logging won't work
    if (!load_dll()) {
        spdlog::error("XeLL init_using_ctx failed to load libxell.dll");
        return false;
    }

    if (!context) {
        spdlog::error("XeLL init_using_ctx called with null context");
        return false;
    }

    ctx = reinterpret_cast<xell_context_handle_t>(context);
    inited_using_context = true;
    last_sleep_mode_valid_ = false;
    for (auto& id : sent_sleep_frame_ids_) id.store(0, std::memory_order_relaxed);
    spdlog::info("XeLL initialized using existing context: {:X}", (uint64_t)ctx);

    o_xellSetLoggingCallback(ctx, XELL_LOGGING_LEVEL_DEBUG, [](const char* message, xell_logging_level_t loggingLevel) {
        switch (loggingLevel) {
            case XELL_LOGGING_LEVEL_DEBUG:
                spdlog::debug("XeLL: {}", message);
            break;
            case XELL_LOGGING_LEVEL_INFO:
                spdlog::info("XeLL: {}", message);
            break;
            case XELL_LOGGING_LEVEL_WARNING:
                spdlog::warn("XeLL: {}", message);
            break;
            case XELL_LOGGING_LEVEL_ERROR:
                spdlog::error("XeLL: {}", message);
            break;
        }
    });

    return true;
}

void XeLL::deinit() {
    exact_frame_pacing_.store(false, std::memory_order_release);
    last_sleep_mode_valid_ = false;
    simulation_start_last_id = 0;
    sleep_last_id = 0;
    for (auto& id : sent_sleep_frame_ids_) id.store(0, std::memory_order_relaxed);
    if (inited_using_context) {
        spdlog::info("XeLL deinit called while inited using context, skipping deinitialization");
        inited_using_context = false;
    } else if (ctx) {
        o_xellDestroyContext(ctx);
        ctx = nullptr;
        spdlog::info("XeLL deinitialized");
    }

    unload_dll();
}

void* XeLL::get_tech_context() {
    return &ctx;
}

void XeLL::get_sleep_status(SleepParams* sleep_params) {
    xell_sleep_params_t xell_sleep_params {};
    auto result = o_xellGetSleepMode(ctx, &xell_sleep_params);

    sleep_params->low_latency_enabled = xell_sleep_params.bLowLatencyMode;
    sleep_params->fullscreen_vrr = true;
    sleep_params->control_panel_vsync_override = false;
}

void XeLL::set_sleep_mode(SleepMode* sleep_mode) {
    xell_sleep_params_t xell_sleep_params {};

    low_latency_enabled = sleep_mode->low_latency_enabled;

    xell_sleep_params.bLowLatencyMode = is_enabled();
    xell_sleep_params.minimumIntervalUs = sleep_mode->minimum_interval_us;
    xell_sleep_params.bLowLatencyBoost = sleep_mode->low_latency_boost;

    if (!last_sleep_mode_valid_ ||
        xell_sleep_params.bLowLatencyMode != last_low_latency_mode_ ||
        xell_sleep_params.minimumIntervalUs != last_minimum_interval_us_ ||
        xell_sleep_params.bLowLatencyBoost != last_low_latency_boost_)
    {
        const auto result = o_xellSetSleepMode(ctx, &xell_sleep_params);
        if (result == XELL_RESULT_SUCCESS) {
            last_sleep_mode_valid_ = true;
            last_low_latency_mode_ = xell_sleep_params.bLowLatencyMode;
            last_minimum_interval_us_ = xell_sleep_params.minimumIntervalUs;
            last_low_latency_boost_ = xell_sleep_params.bLowLatencyBoost;
        }
    }
}

void XeLL::set_marker(IUnknown* pDevice, MarkerParams* marker_params) {
    switch (marker_params->marker_type) {
        case MarkerType::SIMULATION_START:
            simulation_start_last_id = marker_params->frame_id;

            // Legacy fallback only. Startup-locked Hybrid XeLL supplies the
            // exact pacing-owner frame ID explicitly, so marker provenance must
            // never synthesize a second sleep from another frame domain.
            if (!exact_frame_pacing_.load(std::memory_order_acquire) &&
                sleep_last_id + 10 < simulation_start_last_id)
                xell_sleep(marker_params->frame_id);

            add_marker(marker_params->frame_id, XELL_SIMULATION_START);
        break;
        case MarkerType::SIMULATION_END:
            add_marker(marker_params->frame_id, XELL_SIMULATION_END);
        break;
        case MarkerType::RENDERSUBMIT_START:
            add_marker(marker_params->frame_id, XELL_RENDERSUBMIT_START);
        break;
        case MarkerType::RENDERSUBMIT_END:
            add_marker(marker_params->frame_id, XELL_RENDERSUBMIT_END);
        break;
        case MarkerType::PRESENT_START:
            add_marker(marker_params->frame_id, XELL_PRESENT_START);
        break;
        case MarkerType::PRESENT_END:
            add_marker(marker_params->frame_id, XELL_PRESENT_END);
        break;
        // case MarkerType::INPUT_SAMPLE:
        //     add_marker(marker_params->frame_id, XELL_INPUT_SAMPLE);
        // break;
        default:
        break;
    }
}

void XeLL::sleep() {
    // Legacy compatibility path for frontends without an explicit frame ID.
    sleep_last_id = simulation_start_last_id + 1;
    xell_sleep(sleep_last_id);
}

void XeLL::sleep_with_frame_id(uint64_t frame_id) {
    exact_frame_pacing_.store(true, std::memory_order_release);
    sleep_last_id = frame_id;
    xell_sleep(static_cast<uint32_t>(frame_id));
}
