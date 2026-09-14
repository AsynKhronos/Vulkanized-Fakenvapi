#pragma once

#include "low_latency_tech/low_latency_tech.h"
#include "low_latency_tech/ll_vulkanflex.h"
#include "input_arbiter.h"
#include "frame_generation_cadence.h"
#include "frame_generation_cadence_router.h"
#include "hybrid_fusion.h"
#include "policy_engine.h"
#include "openxr_timeline.h"

#include <dxgi.h>
#if _MSC_VER
#include <d3d12.h>
#else
#include "../external/d3d12.h"
#endif

#include "util.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <vulkan/vulkan_core.h>

enum class StructuralStallKind : std::uint8_t;

#define FRAME_REPORTS_BUFFER_SIZE 70
#define NVAPI_BUFFER_SIZE 64

struct FrameReport {
    uint64_t frameID;
    uint64_t inputSampleTime;
    uint64_t simStartTime;
    uint64_t simEndTime;
    uint64_t renderSubmitStartTime;
    uint64_t renderSubmitEndTime;
    uint64_t presentStartTime;
    uint64_t presentEndTime;
    uint64_t driverStartTime;
    uint64_t driverEndTime;
    uint64_t osRenderQueueStartTime;
    uint64_t osRenderQueueEndTime;
    uint64_t gpuRenderStartTime;
    uint64_t gpuRenderEndTime;
    uint32_t gpuActiveRenderTimeUs;
    uint32_t gpuFrameTimeUs;
    uint8_t rsvd[120];
};

class LowLatency {
private:
    std::mutex active_tech_mutex;
    LowLatencyTech* currently_active_tech = nullptr;
    FrameReport frame_reports[FRAME_REPORTS_BUFFER_SIZE]{};
    // Hot-path state is atomically published so unchanged FG/override values do
    // not require the backend mutex on every marker. forced_fg_state_: -1 auto,
    // 0 forced false, 1 forced true.
    std::atomic<std::int8_t> forced_fg_state_{-1};
    std::atomic<bool> fg_{false};
    std::atomic<std::uint8_t> applied_fg_state_{0xff};
    std::atomic<std::uint8_t> applied_override_{0xff};
    // Async OOB-present cadence uses TLS history behind a lock-free stream
    // owner. Thread migration can recover after bounded render-id lag without
    // putting a locked RMW on the ordinary presentation path.
    policy::FrameGenerationCadenceRouter fg_cadence_router_{};
    uint32_t delay_deinit = 0;
    policy::Backend active_backend = policy::Backend::Auto;
    policy::GraphicsApi active_api = policy::GraphicsApi::D3D12;
    IUnknown* active_d3d_device = nullptr;
    VkDevice active_vk_device = VK_NULL_HANDLE;
    uint64_t applied_routing_signature = 0;
    uint64_t pending_routing_signature = 0;
    policy::InputArbiter input_arbiter{};
    policy::HybridFusion hybrid_fusion_{};
    xrflex::XrFlexTimeline xr_timeline_{};
    std::atomic<bool> xr_timeline_logged_{false};
    std::atomic<bool> xr_correlation_logged_{false};
    std::atomic<bool> xr_clock_logged_{false};
    std::atomic<policy::InputFrontend> selected_input{policy::InputFrontend::Auto};
    std::atomic<policy::Backend> active_backend_published_{policy::Backend::Auto};
    std::atomic<policy::GraphicsApi> active_api_published_{policy::GraphicsApi::D3D12};
    std::atomic<LowLatencyTech*> active_tech_published_{nullptr};
    std::atomic<IUnknown*> active_d3d_device_published_{nullptr};
    std::atomic<VkDevice> active_vk_device_published_{VK_NULL_HANDLE};
    std::atomic<std::uint64_t> applied_routing_signature_published_{0};
    std::atomic<bool> latency_not_ready_logged_{false};
    std::atomic<bool> shadow_vulkan_logged_{false};

    // Optional translation telemetry plane. It is never the pacing owner when
    // a stronger D3D12 executor (normally XeLL) is active. The pointer is
    // atomically published so marker/queue hot paths never take the backend
    // mutex merely to feed VKD3D transport observations.
    VulkanFlex transport_observer_storage_{};
    std::atomic<LowLatencyTech*> transport_observer_published_{nullptr};

    struct FrontendControlState {
        std::atomic<uint64_t> sleep_mode{0};
    };
    std::array<FrontendControlState, 4> frontend_control_state_{};
    std::array<std::atomic<uint64_t>, 4> frontend_sleep_sequence_{};
    std::atomic<uint64_t> applied_sleep_mode_{0};
    std::atomic<policy::InputFrontend> applied_sleep_frontend_{policy::InputFrontend::Auto};
    std::atomic<std::uint64_t> applied_hybrid_signature_{0};
    // Once Hybrid startup ownership is frozen, marker arbitration is immutable.
    // This flag turns subsequent callbacks into a minimal fixed-route hot path.
    std::atomic<bool> hybrid_hotpath_locked_{false};
    // Control changes are rare; markers are not. Resolve fused control only
    // when a frontend actually publishes a new control packet.
    std::atomic<std::uint64_t> frontend_control_epoch_{1};
    std::atomic<std::uint64_t> applied_control_epoch_{0};

    // Once set, it will be used for all init attempts
    void* forced_low_latency_context = nullptr;
    Mode forced_low_latency_tech = Mode::LatencyFlex;

    void update_effective_fg_state();
    [[nodiscard]] bool observe_input(policy::InputFrontend frontend, MarkerType marker, uint64_t frame_id);
    [[nodiscard]] bool input_can_drive(policy::InputFrontend frontend) const noexcept;
    [[nodiscard]] bool frontend_selection_enabled(policy::InputFrontend frontend) const noexcept;
    [[nodiscard]] bool frontend_requested_enabled(
        policy::InputFrontend frontend, bool* enabled) const noexcept;
    [[nodiscard]] std::uint8_t frontend_selection_mask() const noexcept;
    [[nodiscard]] bool observe_sleep_input(policy::InputFrontend frontend, uint64_t frame_id, uint64_t* effective_frame_id = nullptr);
    void update_enabled_override();
    void store_frontend_sleep_mode(policy::InputFrontend frontend, const SleepMode& mode) noexcept;
    void apply_frontend_sleep_mode(policy::InputFrontend frontend);
    [[nodiscard]] bool hybrid_execution_active() const noexcept;
    [[nodiscard]] static std::uint8_t hybrid_control_fields(policy::InputFrontend frontend) noexcept;
    [[nodiscard]] bool hybrid_accept_marker(
        policy::InputFrontend frontend, MarkerType marker, uint64_t frame_id,
        uint64_t* canonical_frame_id = nullptr) noexcept;

    // D3D
    [[nodiscard]] policy::GraphicsApi cached_d3d_api(IUnknown* pDevice) const noexcept;
    bool update_low_latency_tech(IUnknown* pDevice, std::optional<policy::GraphicsApi> api_hint = std::nullopt);
    bool init_translation_observer(IUnknown* pDevice, policy::Backend executor_backend);
    void deinit_translation_observer() noexcept;
    void get_latency_result(NV_LATENCY_RESULT_PARAMS* pGetLatencyParams);
    void add_marker_to_report(NV_LATENCY_MARKER_PARAMS *pSetLatencyMarkerParams);
    
    // Vulkan
    bool update_low_latency_tech(HANDLE vkDevice);
    [[nodiscard]] bool vulkan_frontend_shadowed_by_translation() const noexcept;
    bool ignore_shadow_vulkan_frontend(const char* operation) noexcept;
    void get_latency_result(NV_VULKAN_LATENCY_RESULT_PARAMS *pGetLatencyParams);
    void add_vulkan_marker_to_report(MarkerType marker, uint64_t frame_id);
    void get_vulkan_latency_timings(VkGetLatencyMarkerInfoNV* marker_info);

public:
    LowLatency() = default;
    ~LowLatency() { /*deinit_current_tech();*/ };

    bool deinit_current_tech();
    void set_forced_fg(std::optional<bool> forced_fg) {
        forced_fg_state_.store(
            forced_fg.has_value() ? static_cast<std::int8_t>(*forced_fg ? 1 : 0) : static_cast<std::int8_t>(-1),
            std::memory_order_release);
        applied_fg_state_.store(0xff, std::memory_order_release);
    };
    void set_fg_type(bool interpolated, uint64_t frame_id) {
        if (auto* tech = active_tech_published_.load(std::memory_order_acquire))
            tech->set_fg_type(interpolated, frame_id);
    }
    bool get_low_latency_context(void** low_latency_context, Mode* low_latency_tech);
    bool set_low_latency_context(void* low_latency_context, Mode low_latency_tech);

    // Shared D3D frontend bridge.  Reflex, XeLL and Anti-Lag 2 all feed these
    // methods; only the currently selected frontend is allowed to drive the
    // single active output backend.
    void FrontendObserveEvidence(policy::InputFrontend frontend, policy::InputEvidence evidence) noexcept;
    bool FrontendSetSleepMode(policy::InputFrontend frontend, policy::GraphicsApi api, IUnknown* pDevice, const SleepMode& mode);
    bool FrontendGetSleepStatus(policy::InputFrontend frontend, policy::GraphicsApi api, IUnknown* pDevice, SleepParams* status);
    bool FrontendSleep(policy::InputFrontend frontend, policy::GraphicsApi api, IUnknown* pDevice, uint64_t frame_id = INVALID_ID);
    bool FrontendSetMarker(policy::InputFrontend frontend, policy::GraphicsApi api, IUnknown* pDevice, MarkerType marker, uint64_t frame_id);
    bool FrontendSetAsyncMarker(policy::InputFrontend frontend, MarkerType marker, uint64_t frame_id, ID3D12CommandQueue* queue = nullptr);
    void FrontendSetFgType(policy::InputFrontend frontend, bool interpolated, uint64_t frame_id);

    // Shared Vulkan frontend bridge. VK_NV_low_latency2 uses these methods when
    // the extension is emulated. Native pass-through uses the observe-only
    // variants so the global input arbiter still enforces one active input
    // without driving a second backend.
    bool VulkanFrontendSetSleepMode(policy::InputFrontend frontend, VkDevice device, const SleepMode& mode);
    bool VulkanFrontendSleep(policy::InputFrontend frontend, VkDevice device, uint64_t frame_id = INVALID_ID);
    bool VulkanFrontendDriveSleep(policy::InputFrontend frontend, VkDevice device, uint64_t frame_id = INVALID_ID);
    bool VulkanFrontendSetMarker(policy::InputFrontend frontend, VkDevice device, MarkerType marker, uint64_t frame_id);
    bool VulkanFrontendObserveSleep(policy::InputFrontend frontend, uint64_t frame_id = INVALID_ID);
    bool VulkanFrontendObserveMarker(policy::InputFrontend frontend, MarkerType marker, uint64_t frame_id);
    void VulkanFrontendGetLatencyTimings(VkGetLatencyMarkerInfoNV* marker_info);

    // VulkanFlex core-WSI fallback. These callbacks are invoked only after the
    // corresponding real Vulkan call succeeds; they never replace WSI work.
    bool VulkanFlexOnAcquire(VkDevice device, VkSwapchainKHR swapchain, std::uint32_t image_index);
    [[nodiscard]] VulkanFlex::PresentTicket VulkanFlexBeginPresent(
        VkDevice device, VkSwapchainKHR swapchain, std::uint32_t image_index) noexcept;
    void VulkanFlexCompletePresent(
        VkDevice device, const VulkanFlex::PresentTicket& ticket,
        std::uint64_t present_id, bool uses_present_id2, bool timing_requested) noexcept;
    void VulkanFlexOnDestroySwapchain(VkDevice device, VkSwapchainKHR swapchain) noexcept;
    void VulkanFlexOnStructuralStall(
        VkDevice device, StructuralStallKind kind, std::uint64_t duration_ns,
        std::uint64_t end_timestamp_ns) noexcept;

    // R4.1 XRFlex observer callbacks. The real OpenXR runtime call has already
    // completed when these run; no method below may sleep, modify XrTime, or
    // become a graphics/XR pacing owner.
    void OpenXROnWaitFrame(
        std::uintptr_t session, std::int64_t predicted_display_time,
        std::int64_t predicted_display_period, bool should_render,
        std::uint64_t return_timestamp_ns,
        std::optional<std::uint64_t> predicted_display_host_ns = std::nullopt) noexcept;
    void OpenXROnBeginFrame(
        std::uintptr_t session, bool discarded_previous,
        std::uint64_t return_timestamp_ns) noexcept;
    void OpenXROnEndFrame(
        std::uintptr_t session, std::int64_t display_time,
        std::uint64_t return_timestamp_ns,
        std::optional<std::uint64_t> submitted_display_host_ns = std::nullopt) noexcept;
    void OpenXROnDestroySession(std::uintptr_t session) noexcept;
    [[nodiscard]] xrflex::TimelineStats OpenXRStats() const noexcept {
        return xr_timeline_.stats();
    }

    [[nodiscard]] policy::InputFrontend selected_input_frontend() const noexcept {
        return selected_input.load(std::memory_order_acquire);
    }

    // D3D
    NvAPI_Status Sleep(IUnknown* pDevice);
    NvAPI_Status SetSleepMode(IUnknown* pDevice, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    NvAPI_Status GetSleepStatus(IUnknown* pDevice, NV_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams);
    NvAPI_Status SetLatencyMarker(IUnknown *pDev, NV_LATENCY_MARKER_PARAMS *pSetLatencyMarkerParams);
    NvAPI_Status SetAsyncFrameMarker(ID3D12CommandQueue *pCommandQueue, NV_ASYNC_FRAME_MARKER_PARAMS *pSetAsyncFrameMarkerParams);
    NvAPI_Status NotifyOutOfBandCommandQueue(ID3D12CommandQueue* pCommandQueue);
    NvAPI_Status GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams);

    // Vulkan
    NvAPI_Status Sleep(HANDLE vkDevice);
    NvAPI_Status SetSleepMode(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    NvAPI_Status GetSleepStatus(HANDLE vkDevice, NV_VULKAN_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams);
    NvAPI_Status SetLatencyMarker(HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS *pSetLatencyMarkerParams);
    NvAPI_Status GetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams);
};