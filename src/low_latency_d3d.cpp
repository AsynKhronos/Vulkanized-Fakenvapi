#include "low_latency.h"

#include "low_latency_tech/ll_antilag2.h"
#include "low_latency_tech/ll_vkd3d_antilag_vk.h"
#include "low_latency_tech/ll_latencyflex.h"
#include "low_latency_tech/ll_vulkanflex.h"
#include "low_latency_tech/ll_xell.h"
#include "vkd3d_vulkan_capabilities.h"
#include "dxvk_vulkan_interop.h"
#include "transport_truth.h"

#include "log.h"
#include "config.h"


namespace {

policy::GraphicsApi detect_d3d_api(IUnknown* device) {
    if (!device) {
        // Existing externally supplied low-latency contexts are DX12-only.
        return policy::GraphicsApi::D3D12;
    }

    ID3D12Device* d3d12 = nullptr;
    const HRESULT hr = device->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void**>(&d3d12));
    if (hr == S_OK && d3d12) {
        d3d12->Release();
        return policy::GraphicsApi::D3D12;
    }
    return policy::GraphicsApi::D3D11;
}

policy::Backend backend_from_mode(Mode mode) {
    switch (mode) {
        case Mode::AntiLag2: return policy::Backend::AntiLag2;
        case Mode::XeLL: return policy::Backend::XeLL;
        case Mode::LatencyFlex: return policy::Backend::LatencyFlex;
        case Mode::AntiLagVk: return policy::Backend::AmdAntiLagVk;
        case Mode::VulkanFlex: return policy::Backend::VulkanFlex;
    }
    return policy::Backend::Auto;
}

LowLatencyTech* make_d3d_backend(policy::Backend backend) {
    switch (backend) {
        case policy::Backend::AmdAntiLagVk: return new Vkd3dAntiLagVk();
        case policy::Backend::AntiLag2: return new AntiLag2();
        case policy::Backend::XeLL: return new XeLL();
        case policy::Backend::LatencyFlex: return new LatencyFlex();
        case policy::Backend::VulkanFlex: return new VulkanFlex();
        default: return nullptr;
    }
}

bool startup_hybrid_configured(policy::GraphicsApi api) noexcept {
    const auto& snapshot = Config::get().snapshot();
    if (!snapshot.hybrid.startup_locked) return false;
    if (api == policy::GraphicsApi::D3D12) {
        const bool xell_path = snapshot.hybrid.xell_fusion;
        const bool vkd3d_vulkanflex_path = snapshot.vulkanflex.enabled &&
            snapshot.vulkanflex.vkd3d_bridge && snapshot.hybrid.vulkan_fusion;
        return xell_path || vkd3d_vulkanflex_path;
    }
    if (api == policy::GraphicsApi::D3D11) {
        return snapshot.vulkanflex.enabled && snapshot.vulkanflex.dxvk_bridge &&
            snapshot.vulkanflex.dxvk_execution != policy::AutoBool::Disabled &&
            snapshot.hybrid.vulkan_fusion;
    }
    return false;
}

bool module_loaded(const char* name) noexcept {
    return name && GetModuleHandleA(name) != nullptr;
}

bool library_present(const char* name) noexcept {
    if (!name) return false;
    char resolved[MAX_PATH]{};
    const DWORD len = SearchPathA(nullptr, name, nullptr, MAX_PATH, resolved, nullptr);
    return len > 0 && len < MAX_PATH;
}

policy::OutputRecognizer detect_d3d_outputs(policy::GraphicsApi api, IUnknown* device) noexcept {
    using namespace policy;

    OutputRecognizer result;
    result.observe(
        Backend::LatencyFlex,
        OutputAvailability::Available,
        50,
        OutputEvidenceSoftwareFallback);

    if (api == GraphicsApi::D3D11) {
        if (module_loaded("amdxx64.dll")) {
            result.observe(Backend::AntiLag2, OutputAvailability::Available, 75,
                           OutputEvidenceModuleLoaded);
        } else if (library_present("amdxx64.dll")) {
            result.observe(Backend::AntiLag2, OutputAvailability::Available, 60,
                           OutputEvidenceLibraryPresent);
        } else {
            result.observe(Backend::AntiLag2, OutputAvailability::Unknown, 10, OutputEvidenceNone);
        }

        dxvk_interop::DxvkVulkanCapabilities dxvk{};
        const bool dxvk_found = dxvk_interop::probe_dxvk_vulkan_capabilities(device, &dxvk);
        if (dxvk_found && dxvk.interop && dxvk.device != VK_NULL_HANDLE) {
            transport_truth::TransportTruth::confirm_dxvk();
            spdlog::info("Transport truth confirmed: dxvk-d3d11 via IDXGIVkInteropDevice");
        }
        const auto& snapshot = Config::get().snapshot();
        const bool bridge = snapshot.vulkanflex.enabled && snapshot.vulkanflex.dxvk_bridge &&
            dxvk_found && dxvk.interop && dxvk.device != VK_NULL_HANDLE;
        bool executable = false;
        if (bridge && snapshot.vulkanflex.dxvk_execution != AutoBool::Disabled) {
            // Auto is safe-by-default: only DXVK's own LL device may own the
            // wait. Enabled permits the embedded fallback when that API is not
            // available.
            executable = dxvk.low_latency_supported ||
                snapshot.vulkanflex.dxvk_execution == AutoBool::Enabled;
        }
        result.observe(
            Backend::VulkanFlex,
            executable ? OutputAvailability::Available :
                (bridge ? OutputAvailability::Unavailable : OutputAvailability::Unavailable),
            executable ? (dxvk.low_latency_supported ? 99 : 94) : 100,
            bridge ? (OutputEvidenceVulkanInterop | OutputEvidenceDeviceInterface |
                      (dxvk.low_latency_supported ? OutputEvidenceNativeFeature : 0))
                   : OutputEvidenceNone);
        return result;
    }

    if (api != GraphicsApi::D3D12) return result;

    if (module_loaded("libxell.dll")) {
        result.observe(Backend::XeLL, OutputAvailability::Available, 95,
                       OutputEvidenceModuleLoaded);
    } else if (library_present("libxell.dll")) {
        result.observe(Backend::XeLL, OutputAvailability::Available, 80,
                       OutputEvidenceLibraryPresent);
    } else {
        // SearchPath can miss custom Wine DLL search locations. Leave XeLL
        // Unknown rather than excluding an initializer that may still succeed.
        result.observe(Backend::XeLL, OutputAvailability::Unknown, 10, OutputEvidenceNone);
    }

    ID3D12Device* d3d12 = nullptr;
    if (!device || FAILED(device->QueryInterface(
            __uuidof(ID3D12Device), reinterpret_cast<void**>(&d3d12))) || !d3d12) {
        result.observe(Backend::AmdAntiLagVk, OutputAvailability::Unavailable, 100,
                       OutputEvidenceNone);
        result.observe(Backend::VulkanFlex, OutputAvailability::Unavailable, 100,
                       OutputEvidenceNone);
        return result;
    }

    AMD::AntiLag2DX12::IAmdExtAntiLagApi* al2 = nullptr;
    const HRESULT al2_qi = d3d12->QueryInterface(
        IID_IAmdExtAntiLagApi, reinterpret_cast<void**>(&al2));
    if (SUCCEEDED(al2_qi) && al2) {
        result.observe(Backend::AntiLag2, OutputAvailability::Available, 95,
                       OutputEvidenceDeviceInterface);
        al2->Release();
    } else if (module_loaded("amdxc64.dll")) {
        result.observe(Backend::AntiLag2, OutputAvailability::Available, 75,
                       OutputEvidenceModuleLoaded);
    } else if (library_present("amdxc64.dll")) {
        result.observe(Backend::AntiLag2, OutputAvailability::Unknown, 60,
                       OutputEvidenceLibraryPresent);
    } else {
        result.observe(Backend::AntiLag2, OutputAvailability::Unknown, 10, OutputEvidenceNone);
    }

    Vkd3dVulkanCapabilities vk_caps{};
    const bool vkd3d = probe_vkd3d_vulkan_capabilities(d3d12, &vk_caps);
    d3d12->Release();

    if (vkd3d && vk_caps.interop && vk_caps.device != VK_NULL_HANDLE) {
        transport_truth::TransportTruth::confirm_vkd3d();
        spdlog::info("Transport truth confirmed: vkd3d-d3d12 via ID3D12DXVKInteropDevice");
    }

    if (!vkd3d || !vk_caps.interop) {
        result.observe(Backend::AmdAntiLagVk, OutputAvailability::Unavailable, 100,
                       OutputEvidenceNone);
        result.observe(Backend::VulkanFlex, OutputAvailability::Unavailable, 100,
                       OutputEvidenceNone);
        return result;
    }

    const auto& snapshot = Config::get().snapshot();
    const bool bridge_usable = snapshot.vulkanflex.enabled && snapshot.vulkanflex.vkd3d_bridge &&
        vk_caps.device != VK_NULL_HANDLE;
    result.observe(
        Backend::VulkanFlex,
        bridge_usable ? OutputAvailability::Available : OutputAvailability::Unavailable,
        bridge_usable ? (vk_caps.interop_v2 ? 99 : 97) : 100,
        bridge_usable
            ? (OutputEvidenceVulkanInterop | OutputEvidenceDeviceInterface | OutputEvidenceSoftwareFallback)
            : OutputEvidenceVulkanInterop);

    result.observe(Backend::AmdAntiLagVk, OutputAvailability::Unknown,
                   vk_caps.interop_v2 ? 78 : 70,
                   OutputEvidenceVulkanInterop | OutputEvidenceDeviceInterface);

    if (vk_caps.amd_anti_lag_extension && vk_caps.amd_anti_lag_feature &&
        vk_caps.device != VK_NULL_HANDLE) {
        result.observe(Backend::AmdAntiLagVk, OutputAvailability::Available, 98,
                       OutputEvidenceVulkanInterop | OutputEvidenceNativeExtension |
                       OutputEvidenceNativeFeature);
    } else {
        result.observe(Backend::AmdAntiLagVk, OutputAvailability::Unavailable, 100,
                       OutputEvidenceVulkanInterop |
                       (vk_caps.amd_anti_lag_extension ? OutputEvidenceNativeExtension : 0));
    }

    return result;
}

void log_output_recognition(policy::GraphicsApi api, const policy::OutputRecognizer& recognizer) {
    const auto log_one = [&](policy::Backend backend) {
        const auto state = recognizer.recognition(backend);
        spdlog::info(
            "Output detector: api={}, backend={}, state={}, confidence={}, evidence=0x{:02x}",
            policy::to_string(api),
            policy::to_string(backend),
            policy::to_string(state.availability),
            state.confidence,
            state.evidence);
    };

    if (api == policy::GraphicsApi::D3D12) {
        log_one(policy::Backend::VulkanFlex);
        log_one(policy::Backend::AmdAntiLagVk);
        log_one(policy::Backend::AntiLag2);
        log_one(policy::Backend::XeLL);
        log_one(policy::Backend::LatencyFlex);
    } else if (api == policy::GraphicsApi::D3D11) {
        log_one(policy::Backend::VulkanFlex);
        log_one(policy::Backend::AntiLag2);
        log_one(policy::Backend::LatencyFlex);
    }
}

} // namespace

void LowLatency::deinit_translation_observer() noexcept {
    auto* observer = transport_observer_published_.exchange(nullptr, std::memory_order_acq_rel);
    if (!observer) return;
    transport_observer_storage_.deinit();
}

bool LowLatency::init_translation_observer(
    IUnknown* pDevice, policy::Backend executor_backend) {
    deinit_translation_observer();

    if (!pDevice || executor_backend == policy::Backend::VulkanFlex ||
        executor_backend == policy::Backend::AmdAntiLagVk) return false;
    const auto& snapshot = Config::get().snapshot();
    if (!snapshot.vulkanflex.enabled) return false;

    if (!transport_observer_storage_.init_observer(pDevice)) return false;

    transport_observer_published_.store(&transport_observer_storage_, std::memory_order_release);
    spdlog::info(
        "VulkanFlex translation observer attached behind executor: {}",
        policy::to_string(executor_backend));
    return true;
}

// private
policy::GraphicsApi LowLatency::cached_d3d_api(IUnknown* pDevice) const noexcept {
    // QueryInterface is control-plane work, not per-frame work. Once a backend
    // is bound to this device, reuse the atomically published API classification.
    if (active_backend_published_.load(std::memory_order_acquire) != policy::Backend::Auto &&
        active_d3d_device_published_.load(std::memory_order_acquire) == pDevice) {
        return active_api_published_.load(std::memory_order_relaxed);
    }
    return detect_d3d_api(pDevice);
}

bool LowLatency::update_low_latency_tech(IUnknown* pDevice, std::optional<policy::GraphicsApi> api_hint) {
    const auto& snapshot = Config::get().snapshot();

    const auto published_backend = active_backend_published_.load(std::memory_order_acquire);
    const auto published_api = active_api_published_.load(std::memory_order_acquire);
    const auto published_device = active_d3d_device_published_.load(std::memory_order_acquire);
    const auto published_signature = applied_routing_signature_published_.load(std::memory_order_acquire);

    policy::GraphicsApi api = policy::GraphicsApi::D3D12;
    if (api_hint.has_value()) {
        api = *api_hint;
    } else if (published_backend != policy::Backend::Auto && published_device == pDevice) {
        api = published_api;
    } else {
        api = detect_d3d_api(pDevice);
    }

    // Steady-state D3D hot path. Backend/device/API/routing are immutable for
    // nearly every frame, so avoid active_tech_mutex entirely until something
    // actually changes. The slow path below owns initialization and teardown.
    if (published_backend != policy::Backend::Auto &&
        published_api == api &&
        (!pDevice || published_device == pDevice) &&
        (!snapshot.general.runtime_switching || published_signature == snapshot.routing_signature)) {
        return true;
    }

    bool has_active = false;
    bool route_changed = false;
    bool api_changed = false;
    bool device_changed = false;
    bool active_is_antilag2 = false;
    {
        std::scoped_lock lock(active_tech_mutex);
        has_active = currently_active_tech != nullptr;
        if (has_active) {
            api_changed = active_api != api;
            device_changed = pDevice && active_d3d_device && active_d3d_device != pDevice;
            route_changed = applied_routing_signature != snapshot.routing_signature;
            active_is_antilag2 = currently_active_tech->get_mode() == Mode::AntiLag2;
        }
    }

    if (has_active && !api_changed && !device_changed) {
        if (!route_changed || !snapshot.general.runtime_switching) {
            if (pDevice && active_d3d_device == nullptr) {
                std::scoped_lock lock(active_tech_mutex);
                active_d3d_device = pDevice;
                active_d3d_device_published_.store(pDevice, std::memory_order_release);
            }
            return true;
        }

        // Preserve the original OptiScaler/FSR-FG grace period when switching
        // away from an active Anti-Lag 2 context.
        if (active_is_antilag2) {
            if (pending_routing_signature != snapshot.routing_signature) {
                pending_routing_signature = snapshot.routing_signature;
                delay_deinit = 50;
                return true;
            }
            if (delay_deinit > 0 && --delay_deinit > 0) {
                return true;
            }
        }
    }

    if (has_active) {
        if (!deinit_current_tech()) {
            spdlog::error("Couldn't deinitialize low latency backend");
            return false;
        }
    }

    std::scoped_lock lock(active_tech_mutex);

    // Externally supplied contexts are an explicit integration contract and
    // remain higher priority than automatic policy selection.
    if (forced_low_latency_context) {
        LowLatencyTech* forced = make_d3d_backend(backend_from_mode(forced_low_latency_tech));
        if (forced && forced->init_using_ctx(forced_low_latency_context)) {
            currently_active_tech = forced;
            active_backend = backend_from_mode(forced_low_latency_tech);
            active_api = api;
            active_d3d_device = pDevice;
            applied_routing_signature = snapshot.routing_signature;
            active_vk_device = VK_NULL_HANDLE;
            active_vk_device_published_.store(VK_NULL_HANDLE, std::memory_order_release);
            active_d3d_device_published_.store(pDevice, std::memory_order_release);
            active_tech_published_.store(forced, std::memory_order_release);
            active_api_published_.store(api, std::memory_order_release);
            applied_routing_signature_published_.store(snapshot.routing_signature, std::memory_order_release);
            active_backend_published_.store(active_backend, std::memory_order_release);
            hybrid_fusion_.set_transport(policy::FrameTransport::D3D12);
            pending_routing_signature = 0;
            spdlog::info("LowLatency backend: {} (external context)", policy::to_string(active_backend));
            return true;
        }
        delete forced;
        spdlog::error("Failed to initialize forced low-latency context");
        return false;
    }

    const auto output_recognizer = detect_d3d_outputs(api, pDevice);
    log_output_recognition(api, output_recognizer);
    const auto candidates = policy::build_backend_candidates(snapshot, api, &output_recognizer);
    for (std::uint8_t i = 0; i < candidates.order.size; ++i) {
        const auto backend = candidates.order.values[i];
        LowLatencyTech* candidate = make_d3d_backend(backend);
        if (!candidate) continue;

        if (candidate->init(pDevice)) {
            currently_active_tech = candidate;
            active_backend = backend;
            active_api = api;
            active_d3d_device = pDevice;
            applied_routing_signature = snapshot.routing_signature;
            active_vk_device = VK_NULL_HANDLE;
            active_vk_device_published_.store(VK_NULL_HANDLE, std::memory_order_release);
            active_d3d_device_published_.store(pDevice, std::memory_order_release);
            active_tech_published_.store(candidate, std::memory_order_release);
            active_api_published_.store(api, std::memory_order_release);
            applied_routing_signature_published_.store(snapshot.routing_signature, std::memory_order_release);
            active_backend_published_.store(active_backend, std::memory_order_release);
            pending_routing_signature = 0;
            delay_deinit = 0;
            if (api == policy::GraphicsApi::D3D12) {
                const bool vkd3d_observer = init_translation_observer(pDevice, backend);
                const bool vkd3d_executor = backend == policy::Backend::VulkanFlex ||
                    backend == policy::Backend::AmdAntiLagVk;
                hybrid_fusion_.set_transport(
                    (vkd3d_observer || vkd3d_executor)
                        ? policy::FrameTransport::Vkd3dD3D12
                        : policy::FrameTransport::D3D12);
            } else if (api == policy::GraphicsApi::D3D11) {
                const bool dxvk_observer = init_translation_observer(pDevice, backend);
                const bool dxvk_executor = backend == policy::Backend::VulkanFlex;
                hybrid_fusion_.set_transport(
                    (dxvk_observer || dxvk_executor)
                        ? policy::FrameTransport::DxvkD3D11
                        : policy::FrameTransport::Unknown);
            }
            spdlog::info("LowLatency backend: {} for {}", policy::to_string(backend), policy::to_string(api));
            return true;
        }

        delete candidate;
    }

    spdlog::error("No usable low-latency backend for {}", policy::to_string(api));
    return false;
}

void LowLatency::get_latency_result(NV_LATENCY_RESULT_PARAMS* pGetLatencyParams) {
    if (pGetLatencyParams->version != NV_LATENCY_RESULT_PARAMS_VER1) {
        spdlog::error("GetLatency: Unsupported version {}", pGetLatencyParams->version);
        return;
    }

    // Assume no frame reports collected yet, report all zeros
    if (frame_reports[FRAME_REPORTS_BUFFER_SIZE - 1].frameID == 0) {
        std::memset(pGetLatencyParams->frameReport, 0, sizeof(pGetLatencyParams->frameReport));
        if (!latency_not_ready_logged_.exchange(true, std::memory_order_acq_rel)) {
            spdlog::info("GetLatency: waiting for enough frame-marker data; suppressing repeated empty-report messages");
        }
        return;
    }

    // Sort frame reports, find the oldest
    size_t minIdx = 0;
    uint64_t minID = frame_reports[0].frameID;
    for (size_t i = 1; i < FRAME_REPORTS_BUFFER_SIZE; i++) {
        if (frame_reports[i].frameID < minID) {
            minID = frame_reports[i].frameID;
            minIdx = i;
        }
    }

    // Copy starting from older before wrapping around
    size_t firstChunk = std::min<uint64_t>(NVAPI_BUFFER_SIZE, FRAME_REPORTS_BUFFER_SIZE - minIdx);
    std::memcpy(pGetLatencyParams->frameReport, frame_reports + minIdx, firstChunk * sizeof(FrameReport));

    // Copy the rest after wrapping around
    if (firstChunk < NVAPI_BUFFER_SIZE) {
        std::memcpy(pGetLatencyParams->frameReport + firstChunk, frame_reports, (NVAPI_BUFFER_SIZE - firstChunk) * sizeof(FrameReport));
    }
}

void LowLatency::add_marker_to_report(NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams) {
    const auto current_timestamp = get_timestamp() / 1000;
    static thread_local uint64_t last_sim_start = current_timestamp;
    static thread_local uint64_t second_last_sim_start = current_timestamp;
    auto* current_report = &frame_reports[pSetLatencyMarkerParams->frameID % FRAME_REPORTS_BUFFER_SIZE];

    if (current_report->frameID != pSetLatencyMarkerParams->frameID) {
        *current_report = FrameReport{};
    }

    current_report->frameID = pSetLatencyMarkerParams->frameID;
    current_report->gpuFrameTimeUs = static_cast<uint32_t>(last_sim_start - second_last_sim_start);
    current_report->gpuActiveRenderTimeUs = 100;
    current_report->driverStartTime = current_timestamp;
    current_report->driverEndTime = current_timestamp + 100;
    current_report->gpuRenderStartTime = current_timestamp;
    current_report->gpuRenderEndTime = current_timestamp + 100;
    current_report->osRenderQueueStartTime = current_timestamp;
    current_report->osRenderQueueEndTime = current_timestamp + 100;
    switch (pSetLatencyMarkerParams->markerType) {
        case SIMULATION_START:
            second_last_sim_start = last_sim_start;
            last_sim_start = current_timestamp;
            current_report->simStartTime = current_timestamp;
            break;
        case SIMULATION_END:
            current_report->simEndTime = current_timestamp;
            break;
        case RENDERSUBMIT_START:
            current_report->renderSubmitStartTime = current_timestamp;
            break;
        case RENDERSUBMIT_END:
            current_report->renderSubmitEndTime = current_timestamp;
            break;
        case PRESENT_START:
            current_report->presentStartTime = current_timestamp;
            break;
        case PRESENT_END:
            current_report->presentEndTime = current_timestamp;
            break;
        case INPUT_SAMPLE:
            current_report->inputSampleTime = current_timestamp;
            break;
        default:
            break;
    }
}

// Shared frontend bridge
bool LowLatency::FrontendSetSleepMode(
    policy::InputFrontend frontend,
    policy::GraphicsApi api,
    IUnknown* pDevice,
    const SleepMode& mode) {
    store_frontend_sleep_mode(frontend, mode);

    // Establish the single execution backend before startup evidence arrives.
    // Without this, the very first Sleep/marker could fall through the legacy
    // selector because Hybrid activation used to depend on an already-created
    // backend. Initialization is control-plane work and happens once; the
    // steady-state path remains lock-free.
    if (active_backend_published_.load(std::memory_order_acquire) == policy::Backend::Auto &&
        startup_hybrid_configured(api) &&
        !update_low_latency_tech(pDevice, api)) {
        return false;
    }

    if (!input_can_drive(frontend)) {
        // Non-pacing frontends never become a second Sleep owner. Under 0.8
        // they may still win independent control aspects (for example Reflex
        // Boost or game-XeLL minimumInterval) while another source owns pacing.
        if (hybrid_execution_active() && active_tech_published_.load(std::memory_order_acquire)) {
            const auto pacing = selected_input.load(std::memory_order_acquire);
            if (pacing != policy::InputFrontend::Auto) apply_frontend_sleep_mode(pacing);
        }
        return true;
    }
    if (!update_low_latency_tech(pDevice, api)) return false;

    update_enabled_override();
    apply_frontend_sleep_mode(frontend);
    return true;
}

bool LowLatency::FrontendGetSleepStatus(
    policy::InputFrontend frontend,
    policy::GraphicsApi api,
    IUnknown* pDevice,
    SleepParams* status) {
    if (!status) return false;

    input_arbiter.note_evidence(frontend, policy::InputEvidenceCapability);

    if (active_backend_published_.load(std::memory_order_acquire) == policy::Backend::Auto &&
        startup_hybrid_configured(api) &&
        !update_low_latency_tech(pDevice, api)) {
        return false;
    }

    if (!input_can_drive(frontend)) {
        *status = SleepParams{};
        status->fullscreen_vrr = true;

        // In startup-locked Hybrid mode, non-pacing frontends are still legitimate fixed
        // information sources. Report their own requested mode instead of
        // pretending that they are disabled merely because they do not own
        // backend Sleep(). This keeps Reflex/XeLL/AL2 metadata production alive
        // while preserving exactly one pacing owner.
        if (hybrid_execution_active()) {
            bool requested_enabled = false;
            if (frontend_requested_enabled(frontend, &requested_enabled)) {
                status->low_latency_enabled = requested_enabled;
            }
            if (frontend == policy::InputFrontend::Reflex) {
                const auto forced = Config::get().get_force_reflex();
                if (forced == ForceReflex::ForceDisable) status->low_latency_enabled = false;
                if (forced == ForceReflex::ForceEnable) status->low_latency_enabled = true;
            }
        }
        return true;
    }

    if (!update_low_latency_tech(pDevice, api)) return false;
    update_enabled_override();
    apply_frontend_sleep_mode(frontend);
    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    if (!tech) return false;
    tech->get_sleep_status(status);
    return true;
}

bool LowLatency::FrontendSleep(
    policy::InputFrontend frontend,
    policy::GraphicsApi api,
    IUnknown* pDevice,
    uint64_t frame_id) {
    if (active_backend_published_.load(std::memory_order_acquire) == policy::Backend::Auto &&
        startup_hybrid_configured(api) &&
        !update_low_latency_tech(pDevice, api)) {
        return false;
    }

    uint64_t effective_frame_id = frame_id;
    if (!observe_sleep_input(frontend, frame_id, &effective_frame_id)) return true;
    if (!update_low_latency_tech(pDevice, api)) return false;

    update_effective_fg_state();
    update_enabled_override();
    apply_frontend_sleep_mode(frontend);
    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    if (!tech) return false;
    if (hybrid_hotpath_locked_.load(std::memory_order_acquire) || hybrid_execution_active())
        tech->sleep_with_frame_id(effective_frame_id);
    else
        tech->sleep();
    return true;
}

bool LowLatency::FrontendSetMarker(
    policy::InputFrontend frontend,
    policy::GraphicsApi api,
    IUnknown* pDevice,
    MarkerType marker,
    uint64_t frame_id) {
    if (active_backend_published_.load(std::memory_order_acquire) == policy::Backend::Auto &&
        startup_hybrid_configured(api) &&
        !update_low_latency_tech(pDevice, api)) {
        return false;
    }

    const bool pacing_callback = observe_input(frontend, marker, frame_id);

    if (pacing_callback) {
        if (!update_low_latency_tech(pDevice, api)) return false;
    } else if ((!hybrid_hotpath_locked_.load(std::memory_order_acquire) && !hybrid_execution_active()) ||
               !active_tech_published_.load(std::memory_order_acquire)) {
        return true;
    }

    // Startup-locked Hybrid freezes one source per semantic aspect before
    // pacing begins. Native frontend IDs are translated to the internal
    // FrameToken sequence before they reach XeLL or any other executor.
    uint64_t canonical_frame_id = frame_id;
    if (!hybrid_accept_marker(frontend, marker, frame_id, &canonical_frame_id)) return true;

    update_effective_fg_state();
    update_enabled_override();
    const auto pacing = selected_input.load(std::memory_order_acquire);
    if (pacing != policy::InputFrontend::Auto) apply_frontend_sleep_mode(pacing);

    MarkerParams marker_params{};
    marker_params.frame_id = canonical_frame_id;
    marker_params.marker_type = marker;
    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    if (!tech) return false;
    tech->set_marker(pDevice, &marker_params);
    if (auto* observer = transport_observer_published_.load(std::memory_order_acquire))
        observer->observe_canonical_marker(&marker_params);
    return true;
}

bool LowLatency::FrontendSetAsyncMarker(
    policy::InputFrontend frontend,
    MarkerType marker,
    uint64_t frame_id,
    ID3D12CommandQueue* queue) {
    if (!hybrid_hotpath_locked_.load(std::memory_order_acquire)) {
        input_arbiter.note_evidence(frontend, policy::InputEvidenceAsyncMarker);
    }
    const bool pacing_callback = observe_input(frontend, marker, frame_id);
    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    auto* observer = transport_observer_published_.load(std::memory_order_acquire);
    // Queue identity is transport metadata, not a semantic marker aspect. Both
    // executor and observer may cache it, but only the executor is allowed to
    // wait and neither path submits Vulkan work.
    if (queue) {
        if (tech) tech->observe_d3d12_queue(queue);
        if (observer) observer->observe_d3d12_queue(queue);
    }
    if (!pacing_callback &&
        ((!hybrid_hotpath_locked_.load(std::memory_order_acquire) && !hybrid_execution_active()) || !tech)) {
        return true;
    }

    uint64_t canonical_frame_id = frame_id;
    if (!tech || !hybrid_accept_marker(frontend, marker, frame_id, &canonical_frame_id)) return true;

    MarkerParams marker_params{};
    marker_params.frame_id = canonical_frame_id;
    marker_params.marker_type = marker;
    tech->set_async_marker_on_queue(queue, &marker_params);
    if (observer) observer->observe_canonical_marker(&marker_params);
    return true;
}

void LowLatency::FrontendSetFgType(
    policy::InputFrontend frontend,
    bool interpolated,
    uint64_t frame_id) {
    if (!hybrid_hotpath_locked_.load(std::memory_order_acquire)) {
        input_arbiter.note_evidence(frontend, policy::InputEvidenceFrameGen);
    }

    std::optional<policy::CanonicalPresentationToken> presentation;
    if (hybrid_hotpath_locked_.load(std::memory_order_acquire)) {
        presentation = hybrid_fusion_.canonicalize_fg_presentation_locked(
            frontend, frame_id, interpolated);
    } else if (hybrid_execution_active()) {
        presentation = hybrid_fusion_.canonicalize_fg_presentation(
            frontend, frame_id, interpolated, Config::get().snapshot(),
            input_arbiter, frontend_selection_mask());
        if (hybrid_fusion_.locked())
            hybrid_hotpath_locked_.store(true, std::memory_order_release);
    } else {
        auto* tech = active_tech_published_.load(std::memory_order_acquire);
        if (!input_can_drive(frontend) || !tech) return;
        tech->set_fg_type(interpolated, frame_id);
        return;
    }

    if (!presentation.has_value()) return;

    PresentationParams params{};
    params.render_frame_id = presentation->render_sequence;
    params.present_frame_id = presentation->sequence;
    params.generation = presentation->generation;
    params.interpolated = presentation->interpolated;

    // This is telemetry/mapping only. The selected executor receives no extra
    // Sleep or marker from a generated presentation.
    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    auto* observer = transport_observer_published_.load(std::memory_order_acquire);
    if (tech) tech->observe_canonical_presentation(&params);
    if (observer && observer != tech) observer->observe_canonical_presentation(&params);

    VFN_HOT_TRACE(
        "FG presentation: render={} present={} generation={} interpolated={}",
        params.render_frame_id, params.present_frame_id, params.generation,
        params.interpolated);
}

// public Reflex frontend
NvAPI_Status LowLatency::Sleep(IUnknown* pDevice) {
    const auto api = cached_d3d_api(pDevice);
    return FrontendSleep(policy::InputFrontend::Reflex, api, pDevice) ? OK() : ERROR();
}

NvAPI_Status LowLatency::SetSleepMode(IUnknown* pDevice, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams) {
    SleepMode sleep_mode{};
    sleep_mode.low_latency_enabled = pSetSleepModeParams->bLowLatencyMode;
    sleep_mode.low_latency_boost = pSetSleepModeParams->bLowLatencyBoost;
    sleep_mode.minimum_interval_us = pSetSleepModeParams->minimumIntervalUs;
    sleep_mode.use_markers_to_optimize = pSetSleepModeParams->bUseMarkersToOptimize;

    const auto api = cached_d3d_api(pDevice);
    return FrontendSetSleepMode(policy::InputFrontend::Reflex, api, pDevice, sleep_mode) ? OK() : ERROR();
}

NvAPI_Status LowLatency::GetSleepStatus(IUnknown* pDevice, NV_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams) {
    SleepParams sleep_params{};
    const auto api = cached_d3d_api(pDevice);
    if (!FrontendGetSleepStatus(policy::InputFrontend::Reflex, api, pDevice, &sleep_params)) return ERROR();

    pGetSleepStatusParams->bLowLatencyMode = sleep_params.low_latency_enabled;
    pGetSleepStatusParams->bFsVrr = sleep_params.fullscreen_vrr;
    pGetSleepStatusParams->bCplVsyncOn = sleep_params.control_panel_vsync_override;
    return OK();
}

NvAPI_Status LowLatency::SetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams) {
    add_marker_to_report(pSetLatencyMarkerParams);

    const auto marker = static_cast<MarkerType>(pSetLatencyMarkerParams->markerType); // enums intentionally match
    const auto api = cached_d3d_api(pDev);
    if (!FrontendSetMarker(policy::InputFrontend::Reflex, api, pDev, marker, pSetLatencyMarkerParams->frameID)) {
        return ERROR();
    }

    VFN_HOT_TRACE("{}: {}", magic_enum::enum_name(marker), pSetLatencyMarkerParams->frameID);
    return NVAPI_OK;
}

NvAPI_Status LowLatency::SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue, NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams) {
    (void)pCommandQueue;
    const auto marker = static_cast<MarkerType>(pSetAsyncFrameMarkerParams->markerType); // enums intentionally match

    if (marker == MarkerType::OUT_OF_BAND_PRESENT_START) {
        thread_local policy::FrameGenerationCadenceRouter::LocalState tls_cadence{};
        const auto thread_token = reinterpret_cast<std::uintptr_t>(&tls_cadence);
        const bool current_fg = fg_.load(std::memory_order_relaxed);
        const auto decision = fg_cadence_router_.observe(
            thread_token, pSetAsyncFrameMarkerParams->frameID, current_fg, tls_cadence);

        bool changed = false;
        if (decision == policy::FrameGenerationCadenceDetector::Decision::Enable && !current_fg) {
            fg_.store(true, std::memory_order_release);
            changed = true;
        } else if (decision == policy::FrameGenerationCadenceDetector::Decision::Disable && current_fg) {
            fg_.store(false, std::memory_order_release);
            changed = true;
        }

        // Backend mutation is event-driven: unchanged cadence does not pay
        // effective-FG state resolution on every presentation.
        if (changed)
            update_effective_fg_state();
    }

    FrontendSetAsyncMarker(policy::InputFrontend::Reflex, marker, pSetAsyncFrameMarkerParams->frameID, pCommandQueue);
    VFN_HOT_TRACE("Async {}: {}", magic_enum::enum_name(marker), pSetAsyncFrameMarkerParams->frameID);
    return NVAPI_OK;
}

NvAPI_Status LowLatency::NotifyOutOfBandCommandQueue(ID3D12CommandQueue* pCommandQueue) {
    if (!pCommandQueue) return ERROR();
    if (auto* tech = active_tech_published_.load(std::memory_order_acquire))
        tech->observe_d3d12_queue(pCommandQueue);
    if (auto* observer = transport_observer_published_.load(std::memory_order_acquire))
        observer->observe_d3d12_queue(pCommandQueue);
    return OK();
}

NvAPI_Status LowLatency::GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams) {
    if (!update_low_latency_tech(pDev)) return ERROR();
    get_latency_result(pGetLatencyParams);
    return OK();
}
