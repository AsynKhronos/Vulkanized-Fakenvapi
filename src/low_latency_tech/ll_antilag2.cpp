#include "ll_antilag2.h"
#include <al2_proxy.h>

namespace {

bool init_dx12_vkd3d_antilag2(AMD::AntiLag2DX12::Context* context, ID3D12Device* device) {
    if (!context || !device || context->m_pAntiLagAPI) return false;

    AMD::AntiLag2DX12::IAmdExtAntiLagApi* api = nullptr;
    const HRESULT qi = device->QueryInterface(
        IID_IAmdExtAntiLagApi,
        reinterpret_cast<void**>(&api));
    if (FAILED(qi) || !api) return false;

    AMD::AntiLag2DX12::APIData_v1 data{};
    data.uiSize = sizeof(data);
    data.uiVersion = 1;
    data.eMode = 2; // disabled during backend initialization
    data.sControlStr = nullptr;
    data.uiControlStrLength = 0;
    data.maxFPS = 0;

    const HRESULT hr = api->UpdateAntiLagState(&data);
    if (FAILED(hr)) {
        api->Release();
        return false;
    }

    context->m_pAntiLagAPI = api;
    context->m_enabled = false;
    context->m_maxFPS = 0;
    return true;
}

} // namespace

inline HRESULT AntiLag2::al2_sleep() {
    int max_fps = 0;

    // TODO: test more, config for this?

    // if (effective_fg_state && minimum_interval_us != 0) {
    //     static uint64_t previous_frame_time = 0;
    //     uint64_t current_time = get_timestamp();
    //     uint64_t frame_time = current_time - previous_frame_time;
    //     if (frame_time < 1000 * minimum_interval_us) {
    //         if (auto res = eepy(minimum_interval_us * 1000 - frame_time); res)
    //             spdlog::error("Sleep command failed: {}", res);
    //     }
    //     previous_frame_time = get_timestamp();
    // } else {
    //     max_fps = minimum_interval_us > 0 ? static_cast<int>((1'000'000ull + minimum_interval_us / 2u) / minimum_interval_us) : 0;
    // }
    max_fps = minimum_interval_us > 0 ? static_cast<int>((1'000'000ull + minimum_interval_us / 2u) / minimum_interval_us) : 0;

    HRESULT result = {};

    // auto pre_sleep = get_timestamp();

    if (dx12_ctx.m_pAntiLagAPI)
        result = AMD::AntiLag2DX12::Update(&dx12_ctx, is_enabled(), max_fps);
    else if (dx11_ctx.m_pAntiLagAPI)
        result = AMD::AntiLag2DX11::Update(&dx11_ctx, is_enabled(), max_fps);

    // log_event("al2_sleep", "{}", get_timestamp() - pre_sleep);

    VFN_HOT_TRACE("AntiLag 2 Call Spot: {}", current_call_spot == CallSpot::SimulationStart ? "SimulationStart" : "SleepCall");

    return result;
}

void AntiLag2::set_fg_type(bool interpolated, uint64_t frame_id) {
    if (effective_fg_state) {
        // log_event("al2_set_fg_type", "{}", reflex_frame_id);
        AMD::AntiLag2DX12::SetFrameGenFrameType(&dx12_ctx, interpolated);
    }
}

bool AntiLag2::init(IUnknown* pDevice) {
    if (dx12_ctx.m_pAntiLagAPI || dx11_ctx.m_pAntiLagAPI) {
        spdlog::warn("Initialization of AntiLag 2 was attempted while the context is not null");
        return false;
    }

    ID3D12Device* d3d12_device = nullptr;
    bool is_d3d12 = false;
    if (pDevice) {
        const HRESULT hr = pDevice->QueryInterface(
            __uuidof(ID3D12Device),
            reinterpret_cast<void**>(&d3d12_device));
        is_d3d12 = hr == S_OK && d3d12_device != nullptr;
    }

    if (is_d3d12) {
        // vkd3d-proton exposes AMD's public IAmdExtAntiLagApi ABI directly on
        // the D3D12 device when VK_AMD_anti_lag is available. Prefer this
        // translation-native path on Wine/Linux instead of trying to obtain a
        // Windows AMD driver extension from amdxc64.dll. The same public IID
        // and vtable are used by AMD's Anti-Lag 2 SDK.
        if (init_dx12_vkd3d_antilag2(&dx12_ctx, d3d12_device)) {
            d3d12_device->Release();
            spdlog::info("AntiLag 2 DX12 initialized via vkd3d-proton/VK_AMD_anti_lag");
            return true;
        }

        // Native Windows fallback. The driver may have loaded after our DLL.
        // Retry installing the frontend hook, then explicitly bypass it for
        // this output context so the backend receives the real AMD interface.
        {
            std::scoped_lock lock(amdxc64_load_mutex);
            if (LoadLibraryA("amdxc64.dll")) amdxc64_load_times++;
        }

        AL2Proxy::hookAntiLag();
        AL2Proxy::disableAl2Kill.store(true, std::memory_order_release);
        const HRESULT init_return = AMD::AntiLag2DX12::Initialize(&dx12_ctx, d3d12_device);
        AL2Proxy::disableAl2Kill.store(false, std::memory_order_release);
        d3d12_device->Release();

        if (SUCCEEDED(init_return) && dx12_ctx.m_pAntiLagAPI) {
            spdlog::info("AntiLag 2 DX12 initialized via native AMD driver interface");
            return true;
        }

        spdlog::info("AntiLag 2 DX12 initialization failed (HRESULT 0x{:08x})",
                     static_cast<unsigned int>(init_return));
        return false;
    }

    // DX11 Anti-Lag 2 does not require the D3D11 device in AMD's public SDK.
    // This also permits a native AL2 DX11 input frontend to drive the common
    // output router even though AmdDxExtCreate11 is invoked with nullptr.
    {
        std::scoped_lock lock(amdxc64_load_mutex);
        LoadLibraryA("amdxx64.dll");
        amdxx64_load_times++;
    }

    AL2Proxy::hookAntiLag();
    AL2Proxy::disableAl2Kill.store(true, std::memory_order_release);
    const HRESULT init_return = AMD::AntiLag2DX11::Initialize(&dx11_ctx);
    AL2Proxy::disableAl2Kill.store(false, std::memory_order_release);

    if (init_return == S_OK) {
        spdlog::info("AntiLag 2 DX11 initialized");
        return true;
    }

    spdlog::info("AntiLag 2 DX11 initialization failed");
    return false;
}

// Only DX12 is supported
bool AntiLag2::init_using_ctx(void* context) {
    if (!context) {
        spdlog::error("AntiLag 2 init_using_ctx called with null context");
        return false;
    }

    // TODO: try to distinguish between DX11 and DX12 contexts
    dx12_ctx = *reinterpret_cast<AMD::AntiLag2DX12::Context*>(context);

    if (dx12_ctx.m_pAntiLagAPI) {
        inited_using_context = true;
        spdlog::info("AntiLag 2 DX12 initialized using existing context");
        return true;
    }

    return false;
}

void AntiLag2::deinit() {
    if (inited_using_context) {
        spdlog::info("AntiLag 2 DX12 deinit called while inited using context, skipping deinitialization");
        inited_using_context = false;
        return;
    }

    {
        std::scoped_lock lock(amdxc64_load_mutex);

        if (const auto module = GetModuleHandleA("amdxc64.dll")) {
            for (uint64_t i = 0; i < amdxc64_load_times; i++) FreeLibrary(module);
        }
        if (const auto module = GetModuleHandleA("amdxx64.dll")) {
            for (uint64_t i = 0; i < amdxx64_load_times; i++) FreeLibrary(module);
        }

        amdxc64_load_times = 0;
        amdxx64_load_times = 0;
    }

    if (dx12_ctx.m_pAntiLagAPI && !AMD::AntiLag2DX12::DeInitialize(&dx12_ctx))
        spdlog::info("AntiLag 2 DX12 deinitialized");

    if (dx11_ctx.m_pAntiLagAPI && !AMD::AntiLag2DX11::DeInitialize(&dx11_ctx))
        spdlog::info("AntiLag 2 DX11 deinitialized");
}

void* AntiLag2::get_tech_context() {
    if (dx12_ctx.m_pAntiLagAPI)
        return &dx12_ctx;
    else if (dx11_ctx.m_pAntiLagAPI)
        return &dx11_ctx;

    return nullptr;
}

void AntiLag2::get_sleep_status(SleepParams* sleep_params) {
    sleep_params->low_latency_enabled = is_enabled();
    sleep_params->fullscreen_vrr = true;
    sleep_params->control_panel_vsync_override = false;
}

void AntiLag2::set_sleep_mode(SleepMode* sleep_mode) {
    // UNUSED:
    // low_latency_boost
    // use_markers_to_optimize

    low_latency_enabled = sleep_mode->low_latency_enabled;
    minimum_interval_us = sleep_mode->minimum_interval_us; // don't convert to fps due to fg fps limit fallback using intervals
}

void AntiLag2::sleep() {
    last_sleep_framecount = simulation_framecount;

    if (current_call_spot == CallSpot::SleepCall)
        al2_sleep();
}

void AntiLag2::set_marker(IUnknown* pDevice, MarkerParams* marker_params) {
    switch(marker_params->marker_type) {
        case MarkerType::SIMULATION_START:
            simulation_framecount++;

            if (last_sleep_framecount + call_spot_switch_threshold < simulation_framecount)
                current_call_spot = CallSpot::SimulationStart;
            else
                current_call_spot = CallSpot::SleepCall;

            if (current_call_spot == CallSpot::SimulationStart)
                al2_sleep();
        break;

        case MarkerType::PRESENT_START:
            if (effective_fg_state) {
                // log_event("al2_end_of_rendering", "{}", reflex_frame_id);
                AMD::AntiLag2DX12::MarkEndOfFrameRendering(&dx12_ctx);
            }
        break;
    }
}

void AntiLag2::set_async_marker(MarkerParams* marker_params) {
    if (marker_params->marker_type == MarkerType::OUT_OF_BAND_PRESENT_START) {
        static uint64_t previous_frame_id = marker_params->frame_id;
        set_fg_type(previous_frame_id == marker_params->frame_id, marker_params->frame_id);
        previous_frame_id = marker_params->frame_id;
    }
}
