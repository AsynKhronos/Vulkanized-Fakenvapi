#include "low_latency.h"

#include "low_latency_tech/ll_antilag_vk.h"
#include "low_latency_tech/ll_latencyflex.h"
#include "low_latency_tech/ll_vulkanflex.h"

#include "log.h"
#include "config.h"
#include "vulkan_device_registry.h"
#include "transport_truth.h"

#include <algorithm>
#include <cstring>

namespace {

LowLatencyTech* make_vulkan_backend(policy::Backend backend) {
    switch (backend) {
        case policy::Backend::AmdAntiLagVk: return new AntiLagVk();
        case policy::Backend::LatencyFlex: return new LatencyFlex();
        case policy::Backend::VulkanFlex: return new VulkanFlex();
        default: return nullptr;
    }
}

policy::OutputRecognizer detect_vulkan_outputs(VkDevice device) noexcept {
    using namespace policy;
    OutputRecognizer result;
    const auto& snapshot = Config::get().snapshot();
    result.observe(Backend::LatencyFlex, OutputAvailability::Available, 50,
                   OutputEvidenceSoftwareFallback);
    const auto native_access = transport_truth::TransportTruth::native_execution_access();
    const bool vulkanflex_available = snapshot.vulkanflex.enabled &&
        native_access != transport_truth::NativeExecutionAccess::Blocked;
    const std::uint16_t vulkanflex_evidence = vulkanflex_available
        ? static_cast<std::uint16_t>(
            OutputEvidenceSoftwareFallback |
            (native_access == transport_truth::NativeExecutionAccess::FullWsi
                ? OutputEvidenceDeviceInterface : OutputEvidenceNone))
        : OutputEvidenceNone;
    result.observe(
        Backend::VulkanFlex,
        vulkanflex_available ? OutputAvailability::Available : OutputAvailability::Unavailable,
        vulkanflex_available
            ? (native_access == transport_truth::NativeExecutionAccess::FullWsi ? 100 : 85)
            : 100,
        vulkanflex_evidence);

    VulkanDeviceState state{};
    if (!VulkanDeviceRegistry::get_state(device, &state)) {
        result.observe(Backend::AmdAntiLagVk, OutputAvailability::Unknown, 10,
                       OutputEvidenceNone);
        return result;
    }

    if (state.amd_anti_lag_enabled && state.anti_lag_update) {
        result.observe(Backend::AmdAntiLagVk, OutputAvailability::Available, 100,
                       OutputEvidenceNativeExtension | OutputEvidenceNativeFeature |
                       OutputEvidenceEntryPoint);
    } else {
        result.observe(Backend::AmdAntiLagVk, OutputAvailability::Unavailable, 100,
                       OutputEvidenceNone);
    }
    return result;
}

void log_vulkan_output_recognition(const policy::OutputRecognizer& recognizer) {
    for (const auto backend : {policy::Backend::VulkanFlex, policy::Backend::AmdAntiLagVk, policy::Backend::LatencyFlex}) {
        const auto state = recognizer.recognition(backend);
        spdlog::info(
            "Output detector: api=vulkan, backend={}, state={}, confidence={}, evidence=0x{:02x}",
            policy::to_string(backend), policy::to_string(state.availability),
            state.confidence, state.evidence);
    }
}

bool startup_vulkan_hybrid_configured() noexcept {
    const auto& snapshot = Config::get().snapshot();
    if (transport_truth::TransportTruth::native_execution_access() ==
        transport_truth::NativeExecutionAccess::Blocked) return false;
    return snapshot.vulkanflex.enabled && snapshot.hybrid.vulkan_fusion &&
           snapshot.hybrid.startup_locked &&
           (snapshot.output.vulkan == policy::Backend::Auto ||
            snapshot.output.vulkan == policy::Backend::VulkanFlex);
}

} // namespace

bool LowLatency::vulkan_frontend_shadowed_by_translation() const noexcept {
    if (active_backend_published_.load(std::memory_order_acquire) == policy::Backend::Auto)
        return false;

    const auto api = active_api_published_.load(std::memory_order_acquire);
    if (api == policy::GraphicsApi::D3D12)
        return transport_truth::TransportTruth::has_confirmed_vkd3d();
    if (api == policy::GraphicsApi::D3D11)
        return transport_truth::TransportTruth::has_confirmed_dxvk();
    return false;
}

bool LowLatency::ignore_shadow_vulkan_frontend(const char* operation) noexcept {
    if (!vulkan_frontend_shadowed_by_translation()) return false;

    if (!shadow_vulkan_logged_.exchange(true, std::memory_order_acq_rel)) {
        const auto truth = transport_truth::TransportTruth::snapshot();
        spdlog::info(
            "Vulkan frontend shadowed by authoritative translation route: active_api={}, transport={}, operation={}; keeping existing D3D pacing owner",
            policy::to_string(active_api_published_.load(std::memory_order_relaxed)),
            transport_truth::to_string(truth.transport),
            operation ? operation : "unknown");
    }
    return true;
}

// private
bool LowLatency::update_low_latency_tech(HANDLE vkDevice) {
    const auto& snapshot = Config::get().snapshot();
    constexpr auto api = policy::GraphicsApi::Vulkan;
    const auto device = reinterpret_cast<VkDevice>(vkDevice);

    if (vulkan_frontend_shadowed_by_translation())
        return true;
    if (transport_truth::TransportTruth::native_execution_access() ==
        transport_truth::NativeExecutionAccess::Blocked) {
        return false;
    }

    const auto published_backend = active_backend_published_.load(std::memory_order_acquire);
    const auto published_api = active_api_published_.load(std::memory_order_acquire);
    const auto published_device = active_vk_device_published_.load(std::memory_order_acquire);
    const auto published_signature = applied_routing_signature_published_.load(std::memory_order_acquire);

    // Vulkan steady-state fast path. This mirrors the D3D path: after backend
    // initialization, every frame avoids active_tech_mutex unless API/device or
    // runtime routing actually changed.
    if (published_backend != policy::Backend::Auto &&
        published_api == api &&
        published_device == device &&
        (!snapshot.general.runtime_switching || published_signature == snapshot.routing_signature)) {
        return true;
    }

    bool has_active = false;
    bool needs_reinit = false;
    {
        std::scoped_lock lock(active_tech_mutex);
        has_active = currently_active_tech != nullptr;
        if (has_active) {
            const bool api_changed = active_api != api;
            const bool device_changed = active_vk_device != VK_NULL_HANDLE && active_vk_device != device;
            const bool route_changed = applied_routing_signature != snapshot.routing_signature;
            needs_reinit = api_changed || device_changed ||
                (route_changed && snapshot.general.runtime_switching);
        }
    }

    if (has_active && !needs_reinit) {
        // A legacy active Vulkan session may predate the published-device fast
        // path. Publish it once, then all later callbacks stay lock-free.
        active_vk_device_published_.store(device, std::memory_order_release);
        return true;
    }

    if (has_active && !deinit_current_tech()) {
        spdlog::error("Couldn't deinitialize Vulkan low-latency backend");
        return false;
    }

    std::scoped_lock lock(active_tech_mutex);
    const auto output_recognizer = detect_vulkan_outputs(device);
    log_vulkan_output_recognition(output_recognizer);
    const auto candidates = policy::build_backend_candidates(snapshot, api, &output_recognizer);
    for (std::uint8_t i = 0; i < candidates.order.size; ++i) {
        const auto backend = candidates.order.values[i];
        LowLatencyTech* candidate = make_vulkan_backend(backend);
        if (!candidate) continue;

        IUnknown* init_arg = backend == policy::Backend::VulkanFlex
            ? nullptr
            : reinterpret_cast<IUnknown*>(device);
        if (candidate->init(init_arg)) {
            currently_active_tech = candidate;
            active_backend = backend;
            active_api = api;
            active_d3d_device = nullptr;
            active_vk_device = device;
            applied_routing_signature = snapshot.routing_signature;
            active_d3d_device_published_.store(nullptr, std::memory_order_release);
            active_vk_device_published_.store(device, std::memory_order_release);
            active_tech_published_.store(candidate, std::memory_order_release);
            active_api_published_.store(api, std::memory_order_release);
            applied_routing_signature_published_.store(snapshot.routing_signature, std::memory_order_release);
            // Publish backend last. Readers treat non-Auto as the commit marker
            // for the fully initialized fast-path tuple above.
            active_backend_published_.store(active_backend, std::memory_order_release);
            hybrid_fusion_.set_transport(policy::FrameTransport::NativeVulkan);
            pending_routing_signature = 0;
            delay_deinit = 0;
            spdlog::info("LowLatency backend: {} for vulkan", policy::to_string(backend));
            return true;
        }

        delete candidate;
    }

    spdlog::error("No usable low-latency backend for Vulkan");
    return false;
}

void LowLatency::get_latency_result(NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams) {
    if (pGetLatencyParams->version != NV_VULKAN_LATENCY_RESULT_PARAMS_VER1) {
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

    // gpuFrameTimeUs and gpuActiveRenderTimeUs are missing in the vk struct
    for (auto i = 0; i < NVAPI_BUFFER_SIZE; i++) {
        std::memset(&pGetLatencyParams->frameReport[i].rsvd[0], 0, sizeof(FrameReport::gpuFrameTimeUs) + sizeof(FrameReport::gpuActiveRenderTimeUs));
    }
}

void LowLatency::add_vulkan_marker_to_report(MarkerType marker, uint64_t frame_id) {
    // Exactly one clock read per marker. Older code performed a second read in
    // every switch arm, adding avoidable timing overhead to the frame path.
    const auto current_timestamp = get_timestamp() / 1000;
    static thread_local uint64_t last_sim_start = current_timestamp;
    static thread_local uint64_t second_last_sim_start = current_timestamp;
    auto* current_report = &frame_reports[frame_id % FRAME_REPORTS_BUFFER_SIZE];

    if (current_report->frameID != frame_id)
        *current_report = FrameReport{};

    current_report->frameID = frame_id;
    current_report->gpuFrameTimeUs = static_cast<uint32_t>(last_sim_start - second_last_sim_start);
    current_report->gpuActiveRenderTimeUs = 100;
    current_report->driverStartTime = current_timestamp;
    current_report->driverEndTime = current_timestamp + 100;
    current_report->gpuRenderStartTime = current_timestamp;
    current_report->gpuRenderEndTime = current_timestamp + 100;
    current_report->osRenderQueueStartTime = current_timestamp;
    current_report->osRenderQueueEndTime = current_timestamp + 100;

    switch (marker) {
        case MarkerType::SIMULATION_START:
            second_last_sim_start = last_sim_start;
            last_sim_start = current_timestamp;
            current_report->simStartTime = current_timestamp;
            break;
        case MarkerType::SIMULATION_END:
            current_report->simEndTime = current_timestamp;
            break;
        case MarkerType::RENDERSUBMIT_START:
            current_report->renderSubmitStartTime = current_timestamp;
            break;
        case MarkerType::RENDERSUBMIT_END:
            current_report->renderSubmitEndTime = current_timestamp;
            break;
        case MarkerType::PRESENT_START:
        case MarkerType::OUT_OF_BAND_PRESENT_START:
            current_report->presentStartTime = current_timestamp;
            break;
        case MarkerType::PRESENT_END:
        case MarkerType::OUT_OF_BAND_PRESENT_END:
            current_report->presentEndTime = current_timestamp;
            break;
        case MarkerType::INPUT_SAMPLE:
            current_report->inputSampleTime = current_timestamp;
            break;
        default:
            break;
    }
}

void LowLatency::get_vulkan_latency_timings(VkGetLatencyMarkerInfoNV* marker_info) {
    if (!marker_info)
        return;

    std::array<const FrameReport*, FRAME_REPORTS_BUFFER_SIZE> ordered{};
    uint32_t available = 0;
    for (const auto& report : frame_reports) {
        if (report.frameID != 0)
            ordered[available++] = &report;
    }

    std::sort(ordered.begin(), ordered.begin() + available, [](const FrameReport* lhs, const FrameReport* rhs) {
        return lhs->frameID < rhs->frameID;
    });

    if (!marker_info->pTimings) {
        marker_info->timingCount = available;
        return;
    }

    const uint32_t capacity = marker_info->timingCount;
    const uint32_t written = std::min(capacity, available);
    for (uint32_t i = 0; i < written; ++i) {
        auto& timing = marker_info->pTimings[i];
        const auto preserved_p_next = timing.pNext;
        timing = VkLatencyTimingsFrameReportNV{};
        timing.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
        timing.pNext = preserved_p_next;

        const auto& report = *ordered[i];
        timing.presentID = report.frameID;
        timing.inputSampleTimeUs = report.inputSampleTime;
        timing.simStartTimeUs = report.simStartTime;
        timing.simEndTimeUs = report.simEndTime;
        timing.renderSubmitStartTimeUs = report.renderSubmitStartTime;
        timing.renderSubmitEndTimeUs = report.renderSubmitEndTime;
        timing.presentStartTimeUs = report.presentStartTime;
        timing.presentEndTimeUs = report.presentEndTime;
        timing.driverStartTimeUs = report.driverStartTime;
        timing.driverEndTimeUs = report.driverEndTime;
        timing.osRenderQueueStartTimeUs = report.osRenderQueueStartTime;
        timing.osRenderQueueEndTimeUs = report.osRenderQueueEndTime;
        timing.gpuRenderStartTimeUs = report.gpuRenderStartTime;
        timing.gpuRenderEndTimeUs = report.gpuRenderEndTime;
    }
    marker_info->timingCount = written;
}

// Shared Vulkan frontend bridge
bool LowLatency::VulkanFrontendSetSleepMode(
    policy::InputFrontend frontend,
    VkDevice device,
    const SleepMode& mode) {
    if (ignore_shadow_vulkan_frontend("set-sleep-mode")) return true;
    store_frontend_sleep_mode(frontend, mode);

    // For VulkanFlex Hybrid, create the one execution backend before the
    // startup evidence window locks.  Core WSI can keep pacing during that
    // window while explicit frontends are scored without becoming a second
    // scheduler.
    if (active_backend_published_.load(std::memory_order_acquire) == policy::Backend::Auto &&
        startup_vulkan_hybrid_configured() &&
        !update_low_latency_tech(reinterpret_cast<HANDLE>(device))) {
        return false;
    }

    if (!input_can_drive(frontend)) return true;
    if (!update_low_latency_tech(reinterpret_cast<HANDLE>(device))) return false;

    update_enabled_override();
    apply_frontend_sleep_mode(frontend);
    return true;
}

bool LowLatency::VulkanFrontendSleep(
    policy::InputFrontend frontend,
    VkDevice device,
    uint64_t frame_id) {
    if (ignore_shadow_vulkan_frontend("sleep")) return true;
    if (active_backend_published_.load(std::memory_order_acquire) == policy::Backend::Auto &&
        startup_vulkan_hybrid_configured() &&
        !update_low_latency_tech(reinterpret_cast<HANDLE>(device))) {
        return false;
    }

    uint64_t effective_frame_id = frame_id;
    if (!observe_sleep_input(frontend, frame_id, &effective_frame_id)) return true;
    return VulkanFrontendDriveSleep(frontend, device, effective_frame_id);
}

bool LowLatency::VulkanFrontendDriveSleep(
    policy::InputFrontend frontend,
    VkDevice device,
    uint64_t frame_id) {
    if (ignore_shadow_vulkan_frontend("drive-sleep")) return true;
    if (!input_can_drive(frontend)) return true;
    if (!update_low_latency_tech(reinterpret_cast<HANDLE>(device))) return false;

    update_effective_fg_state();
    update_enabled_override();
    apply_frontend_sleep_mode(frontend);
    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    if (!tech) return false;
    if (hybrid_hotpath_locked_.load(std::memory_order_acquire) || hybrid_execution_active())
        tech->sleep_with_frame_id(frame_id);
    else
        tech->sleep();
    return true;
}

bool LowLatency::VulkanFrontendSetMarker(
    policy::InputFrontend frontend,
    VkDevice device,
    MarkerType marker,
    uint64_t frame_id) {
    if (ignore_shadow_vulkan_frontend("marker")) return true;
    add_vulkan_marker_to_report(marker, frame_id);

    if (active_backend_published_.load(std::memory_order_acquire) == policy::Backend::Auto &&
        startup_vulkan_hybrid_configured() &&
        !update_low_latency_tech(reinterpret_cast<HANDLE>(device))) {
        return false;
    }

    const bool pacing_callback = observe_input(frontend, marker, frame_id);
    if (pacing_callback) {
        if (!update_low_latency_tech(reinterpret_cast<HANDLE>(device))) return false;
    } else if ((!hybrid_hotpath_locked_.load(std::memory_order_acquire) && !hybrid_execution_active()) ||
               !active_tech_published_.load(std::memory_order_acquire)) {
        return true;
    }

    // Vulkan uses the same transport-neutral FrameToken sequence as D3D12.
    // Native marker IDs remain source metadata and never leak into the executor.
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
    tech->set_marker(reinterpret_cast<IUnknown*>(device), &marker_params);
    return true;
}

bool LowLatency::VulkanFrontendObserveSleep(policy::InputFrontend frontend, uint64_t frame_id) {
    if (ignore_shadow_vulkan_frontend("observe-sleep")) return true;
    return observe_sleep_input(frontend, frame_id);
}

bool LowLatency::VulkanFrontendObserveMarker(
    policy::InputFrontend frontend,
    MarkerType marker,
    uint64_t frame_id) {
    if (ignore_shadow_vulkan_frontend("observe-marker")) return true;
    add_vulkan_marker_to_report(marker, frame_id);
    return observe_input(frontend, marker, frame_id);
}

void LowLatency::VulkanFrontendGetLatencyTimings(VkGetLatencyMarkerInfoNV* marker_info) {
    get_vulkan_latency_timings(marker_info);
}

bool LowLatency::VulkanFlexOnAcquire(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t image_index) {
    if (vulkan_frontend_shadowed_by_translation()) return true;
    const auto& snapshot = Config::get().snapshot();
    if (!snapshot.vulkanflex.enabled || !snapshot.vulkanflex.core_pacing)
        return true;

    const bool forced = snapshot.output.vulkan == policy::Backend::VulkanFlex;

    // A real native VK_NV_low_latency2 implementation already owns pacing.
    // Auto mode must not start a second pacing system underneath it. Explicit
    // vulkan=vulkanflex is the deliberate override for targeted testing. This
    // hot check reads only the cached LL2 state; it does not copy device state.
    if (!forced && VulkanDeviceRegistry::has_native_nv_low_latency2(device) &&
        snapshot.vulkan.prefer_native_extensions) {
        return true;
    }

    if (!update_low_latency_tech(reinterpret_cast<HANDLE>(device)))
        return false;
    if (active_backend_published_.load(std::memory_order_acquire) != policy::Backend::VulkanFlex)
        return true;

    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    auto* vulkanflex = static_cast<VulkanFlex*>(tech);
    if (vulkanflex)
        vulkanflex->on_acquire(device, swapchain, image_index);
    return true;
}

VulkanFlex::PresentTicket LowLatency::VulkanFlexBeginPresent(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t image_index) noexcept {
    VulkanFlex::PresentTicket ticket{};
    if (active_api_published_.load(std::memory_order_acquire) != policy::GraphicsApi::Vulkan ||
        active_vk_device_published_.load(std::memory_order_acquire) != device ||
        active_backend_published_.load(std::memory_order_acquire) != policy::Backend::VulkanFlex) {
        return ticket;
    }

    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    if (auto* vulkanflex = static_cast<VulkanFlex*>(tech))
        return vulkanflex->begin_present(swapchain, image_index);
    return ticket;
}

void LowLatency::VulkanFlexCompletePresent(
    VkDevice device, const VulkanFlex::PresentTicket& ticket,
    std::uint64_t present_id, bool uses_present_id2, bool timing_requested) noexcept {
    if (!ticket ||
        active_api_published_.load(std::memory_order_acquire) != policy::GraphicsApi::Vulkan ||
        active_vk_device_published_.load(std::memory_order_acquire) != device ||
        active_backend_published_.load(std::memory_order_acquire) != policy::Backend::VulkanFlex) {
        return;
    }

    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    if (auto* vulkanflex = static_cast<VulkanFlex*>(tech))
        vulkanflex->complete_present(
            device, ticket, present_id, uses_present_id2, timing_requested);
}

void LowLatency::VulkanFlexOnDestroySwapchain(
    VkDevice device, VkSwapchainKHR swapchain) noexcept {
    if (active_api_published_.load(std::memory_order_acquire) != policy::GraphicsApi::Vulkan ||
        active_vk_device_published_.load(std::memory_order_acquire) != device ||
        active_backend_published_.load(std::memory_order_acquire) != policy::Backend::VulkanFlex) {
        return;
    }

    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    if (auto* vulkanflex = static_cast<VulkanFlex*>(tech))
        vulkanflex->on_destroy_swapchain(swapchain);
}

void LowLatency::VulkanFlexOnStructuralStall(
    VkDevice device, StructuralStallKind kind, std::uint64_t duration_ns,
    std::uint64_t end_timestamp_ns) noexcept {
    if (active_api_published_.load(std::memory_order_acquire) != policy::GraphicsApi::Vulkan ||
        active_vk_device_published_.load(std::memory_order_acquire) != device ||
        active_backend_published_.load(std::memory_order_acquire) != policy::Backend::VulkanFlex) {
        return;
    }

    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    if (auto* vulkanflex = static_cast<VulkanFlex*>(tech))
        vulkanflex->observe_structural_stall(kind, duration_ns, end_timestamp_ns);
}

// public NVAPI Vulkan Reflex frontend
NvAPI_Status LowLatency::Sleep(HANDLE vkDevice) {
    return VulkanFrontendSleep(
        policy::InputFrontend::Reflex,
        reinterpret_cast<VkDevice>(vkDevice)) ? OK() : ERROR();
}

NvAPI_Status LowLatency::SetSleepMode(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams) {
    SleepMode sleep_mode{};
    sleep_mode.low_latency_enabled = pSetSleepModeParams->bLowLatencyMode;
    sleep_mode.low_latency_boost = pSetSleepModeParams->bLowLatencyBoost;
    sleep_mode.minimum_interval_us = pSetSleepModeParams->minimumIntervalUs;
    sleep_mode.use_markers_to_optimize = true;

    return VulkanFrontendSetSleepMode(
        policy::InputFrontend::Reflex,
        reinterpret_cast<VkDevice>(vkDevice),
        sleep_mode) ? OK() : ERROR();
}

NvAPI_Status LowLatency::GetSleepStatus(HANDLE vkDevice, NV_VULKAN_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams) {
    if (ignore_shadow_vulkan_frontend("get-sleep-status")) {
        SleepParams sleep_params{};
        if (auto* tech = active_tech_published_.load(std::memory_order_acquire))
            tech->get_sleep_status(&sleep_params);
        pGetSleepStatusParams->bLowLatencyMode = sleep_params.low_latency_enabled;
        return OK();
    }
    if (!input_can_drive(policy::InputFrontend::Reflex)) {
        pGetSleepStatusParams->bLowLatencyMode = false;
        return OK();
    }
    if (!update_low_latency_tech(vkDevice)) return ERROR();

    SleepParams sleep_params{};
    apply_frontend_sleep_mode(policy::InputFrontend::Reflex);
    auto* tech = active_tech_published_.load(std::memory_order_acquire);
    if (!tech) return ERROR();
    tech->get_sleep_status(&sleep_params);
    pGetSleepStatusParams->bLowLatencyMode = sleep_params.low_latency_enabled;
    return OK();
}

NvAPI_Status LowLatency::SetLatencyMarker(HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams) {
    const auto marker = static_cast<MarkerType>(pSetLatencyMarkerParams->markerType); // legacy NVAPI enums intentionally match
    if (!VulkanFrontendSetMarker(
            policy::InputFrontend::Reflex,
            reinterpret_cast<VkDevice>(vkDevice),
            marker,
            pSetLatencyMarkerParams->frameID)) {
        return ERROR();
    }

    VFN_HOT_TRACE("{}: {}", magic_enum::enum_name(marker), pSetLatencyMarkerParams->frameID);
    return NVAPI_OK;
}

NvAPI_Status LowLatency::GetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams) {
    (void)vkDevice;
    get_latency_result(pGetLatencyParams);
    return OK();
}
