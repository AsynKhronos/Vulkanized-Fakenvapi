#pragma once

#include "low_latency_tech.h"
#include "../external/latencyflex.h"
#include "../fixed_mpmc_ring.h"
#include "../vkd3d_vulkan_interop.h"
#include "../dxvk_vulkan_interop.h"
#include "../vulkan_present_precision.h"
#include "../vulkan_device_registry.h"
#include "../vulkan_queue_pressure.h"
#include "../vulkan_structural_stall.h"
#include "../completion_feedback_policy.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vulkan/vulkan_core.h>

// VulkanFlex is a translation-aware, cross-vendor Vulkan latency orchestrator.
// Native Vulkan can use the embedded LatencyFleX scheduler and core WSI bridge;
// VKD3D/DXVK use cooperative COM interop without Vulkan detours. DXVK may own
// the actual wait through ID3DLowLatencyDevice. VulkanFlex never owns a Vulkan
// queue or command buffer.
class VulkanFlex final : public LowLatencyTech {
public:
    // Two-phase WSI present ticket. The image->frame mapping is claimed before
    // the real vkQueuePresentKHR call and completed only after the driver reports
    // success. This prevents a fast acquire on another thread from recycling an
    // image slot before the post-present hook consumes the old frame mapping.
    struct PresentTicket {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::uint64_t frame_id = 0;
        std::uint64_t epoch = 0;
        std::uint32_t image_index = 0;
        std::uint8_t lane_index = 0xff;

        [[nodiscard]] explicit operator bool() const noexcept {
            return swapchain != VK_NULL_HANDLE && frame_id != 0 && epoch != 0 &&
                   lane_index != 0xff;
        }
    };

    static constexpr std::size_t kTrackedSwapchainLanes = 16;

private:
    enum class ExplicitPacing : std::uint8_t { None, Marker, Sleep };
    enum class Transport : std::uint8_t { NativeVulkan, Vkd3dD3D12, DxvkD3D11 };
    enum class Role : std::uint8_t { Executor, Observer, Delegate };

    static constexpr std::size_t kSwapchainLanes = kTrackedSwapchainLanes;
    static constexpr std::size_t kImagesPerLane = 64;
    static constexpr std::size_t kFeedbackCapacity = 64;
    static constexpr std::size_t kPrecisionPending = 8;
    static constexpr std::uint8_t kPrecisionMaxPolls = 6;
    static constexpr std::uint8_t kPrecisionWaitProbeBudget = 4;
    static constexpr std::size_t kStructuralStallSlots = 8;
    static constexpr std::uint64_t kStallSlotPublishing = UINT64_MAX;

    struct PrecisionPending {
        // present_id is the publication/ownership word. Writers publish it last;
        // pollers claim a sample by CASing it back to zero.
        std::atomic<std::uint64_t> present_id{0};
        std::atomic<std::uint64_t> frame_id{0};
        std::atomic<std::uint64_t> epoch{0};
        std::atomic<std::uint64_t> fallback_timestamp_ns{0};
        std::atomic<std::uint8_t> flags{0};
        std::atomic<std::uint8_t> polls{0};
    };

    struct SwapchainLane {
        std::atomic<VkSwapchainKHR> swapchain{VK_NULL_HANDLE};
        std::array<std::atomic<std::uint64_t>, kImagesPerLane> frame_ids{};
        std::array<std::atomic<std::uint64_t>, kImagesPerLane> frame_epochs{};
        std::array<PrecisionPending, kPrecisionPending> precision_pending{};
        // Advisory occupancy count for the precision ring. The present_id word
        // remains authoritative; this count only lets the common no-pending
        // acquire path avoid registry lookups and eight atomic slot probes.
        std::atomic<std::uint32_t> precision_pending_count{0};
        std::atomic<std::uint8_t> precision_probe_cursor{0};

        // Precision dispatch/swapchain capabilities are immutable for the
        // lifetime of a VkSwapchainKHR. Cache them once per lane instead of
        // re-entering the global registry on every Present/Acquire probe.
        // 0=uninitialized, 1=publishing, 2=ready, 3=unavailable.
        std::atomic<std::uint8_t> precision_profile_state{0};
        VkDevice precision_device = VK_NULL_HANDLE;
        VkSwapchainCreateFlagsKHR precision_swapchain_flags = 0;
        VulkanPresentPrecisionDispatch precision_dispatch{};
        std::atomic<std::uint8_t> best_precision_tier{0};
        std::atomic<std::uint32_t> precise_completions{0};
        std::atomic<std::uint32_t> fallback_completions{0};
        std::atomic<std::uint32_t> max_pending_seen{0};
        std::atomic<std::uint64_t> pressure_epoch{0};
    };

    struct Feedback {
        std::uint64_t frame_id = 0;
        std::uint64_t timestamp_ns = 0;
        std::uint64_t epoch = 0;
    };

    struct StructuralStallSlot {
        // end_timestamp_ns is the publication word. UINT64_MAX means a writer
        // owns the slot; readers ignore it until the final release-store.
        std::atomic<std::uint64_t> end_timestamp_ns{0};
        std::atomic<std::uint64_t> duration_ns{0};
        std::atomic<std::uint64_t> epoch{0};
        std::atomic<std::uint8_t> kind{static_cast<std::uint8_t>(StructuralStallKind::GraphicsPipeline)};
    };

    struct StructuralEvidenceBatch {
        std::uint64_t duration_ns = 0;
        std::uint64_t latest_end_ns = 0;
        StructuralStallKind strongest_kind = StructuralStallKind::GraphicsPipeline;
        std::uint32_t events = 0;
    };

    lfx::LatencyFleX ctx_{};
    std::array<SwapchainLane, kSwapchainLanes> swapchains_{};
    FixedMpmcRing<Feedback, kFeedbackCapacity> feedback_{};

    std::atomic_flag scheduler_gate_ = ATOMIC_FLAG_INIT;
    // Lane creation is rare. Serialize only that slow path so a lane can be
    // fully initialized before swapchain is release-published. Steady-state
    // lane lookup remains lock-free/TLS-cached.
    std::atomic_flag lane_create_gate_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> frame_sequence_{0};
    std::atomic<std::uint64_t> last_started_frame_id_{0};
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<bool> reset_requested_{false};
    std::atomic<VkSwapchainKHR> primary_swapchain_{VK_NULL_HANDLE};
    std::atomic<std::uint64_t> control_word_{0};
    std::atomic<std::uint8_t> override_state_{static_cast<std::uint8_t>(ForceReflex::InGame)};
    std::atomic<std::uint8_t> explicit_pacing_{static_cast<std::uint8_t>(ExplicitPacing::None)};
    std::atomic<bool> core_paced_pending_{false};
    std::atomic<bool> effective_fg_{false};
    std::atomic<std::uint64_t> dropped_feedback_{0};
    std::atomic<std::uint64_t> rejected_feedback_{0};
    std::atomic<std::uint64_t> precision_timing_hits_{0};
    std::atomic<std::uint64_t> precision_wait2_hits_{0};
    std::atomic<std::uint64_t> precision_wait_hits_{0};
    std::atomic<std::uint64_t> precision_fallbacks_{0};
    std::atomic<std::uint64_t> queue_pressure_delay_ns_{0};
    std::atomic<std::uint64_t> queue_started_frames_{0};
    std::atomic<std::uint64_t> queue_completed_frames_{0};
    std::atomic<std::uint64_t> queue_observed_frame_time_ns_{0};
    std::atomic<std::uint64_t> queue_pressure_events_{0};
    std::atomic<std::uint64_t> queue_pressure_total_delay_ns_{0};
    std::atomic<std::uint32_t> queue_pressure_max_pending_{0};
    std::atomic<bool> queue_pressure_trust_logged_{false};

    std::array<StructuralStallSlot, kStructuralStallSlots> structural_stall_slots_{};
    std::atomic<std::uint32_t> structural_stall_cursor_{0};
    std::atomic<std::uint64_t> structural_last_completion_ns_{0};
    std::atomic<std::uint64_t> structural_last_completion_frame_{0};
    std::atomic<std::uint64_t> structural_baseline_ns_{0};
    std::atomic<std::uint32_t> structural_baseline_samples_{0};
    std::atomic<std::uint64_t> structural_pipeline_events_{0};
    std::atomic<std::uint64_t> structural_compensated_events_{0};
    std::atomic<std::uint64_t> structural_compensated_ns_{0};
    std::atomic<std::uint64_t> structural_dropped_events_{0};
    std::atomic<bool> structural_trust_logged_{false};

    // VKD3D cooperative bridge state. The COM interop object is retained for
    // the backend lifetime, while queue/device identities are published
    // atomically after the first authoritative D3D12 queue is observed. No
    // Vulkan queue submission is ever performed by VulkanFlex.
    Transport transport_ = Transport::NativeVulkan;
    Role role_ = Role::Executor;
    VulkanCapabilitySnapshot transport_capabilities_{};
    vkd3d_interop::ID3D12DXVKInteropDeviceMinimal* vkd3d_interop_ = nullptr;
    VkInstance vkd3d_instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice vkd3d_physical_device_ = VK_NULL_HANDLE;
    VkDevice vkd3d_device_ = VK_NULL_HANDLE;
    bool vkd3d_interop_v2_ = false;
    std::atomic<ID3D12CommandQueue*> vkd3d_d3d_queue_{nullptr};
    std::atomic<VkQueue> vkd3d_vk_queue_{VK_NULL_HANDLE};
    std::atomic<std::uint32_t> vkd3d_queue_family_{0xffffffffu};
    std::atomic_flag vkd3d_queue_bind_gate_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> last_bridge_feedback_frame_{INVALID_ID};
    std::atomic<std::uint64_t> observer_frame_count_{0};
    std::atomic<std::uint64_t> observer_present_count_{0};
    std::atomic<std::uint64_t> observer_presentation_count_{0};
    std::atomic<std::uint64_t> observer_generated_present_count_{0};
    std::atomic<std::uint64_t> observer_last_frame_{0};
    std::atomic<std::uint64_t> observer_last_presentation_{0};
    std::atomic<std::uint64_t> observer_last_present_ns_{0};

    // DXVK cooperative bridge. In Delegate role DXVK's own low-latency
    // implementation owns the only CPU wait; VulkanFlex supplies canonical
    // IDs/control and never waits independently.
    dxvk_interop::IDXGIVkInteropDeviceMinimal* dxvk_interop_ = nullptr;
    dxvk_interop::ID3DLowLatencyDeviceMinimal* dxvk_low_latency_ = nullptr;
    VkInstance dxvk_instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice dxvk_physical_device_ = VK_NULL_HANDLE;
    VkDevice dxvk_device_ = VK_NULL_HANDLE;
    VkQueue dxvk_queue_ = VK_NULL_HANDLE;
    std::uint32_t dxvk_queue_family_ = 0xffffffffu;
    bool dxvk_low_latency_supported_ = false;

    std::uint64_t instance_id_ = 0;
    bool core_pacing_enabled_ = true;
    std::uint32_t configured_minimum_interval_us_ = 0;
    std::uint32_t max_sleep_us_ = 50000;
    bool present_precision_enabled_ = true;
    QueuePressureMode queue_pressure_mode_ = QueuePressureMode::Auto;
    std::uint8_t queue_target_presents_ = 1;
    std::uint32_t queue_max_delay_us_ = 2000;
    bool queue_pressure_enabled_ = false;
    StructuralStallMode structural_stall_mode_ = StructuralStallMode::Auto;
    std::uint64_t pipeline_threshold_ns_ = 2'000'000ull;
    std::uint64_t stall_max_compensation_ns_ = 50'000'000ull;
    bool structural_stall_enabled_ = false;
    // True only when VulkanHooks actually intercepted the native VkDevice/WSI
    // chain. A conservative translation bypass can still leave explicit
    // NVAPI/Vulkan marker pacing usable, but WSI-derived features stay off.
    bool native_wsi_authoritative_ = false;

    [[nodiscard]] bool enabled_from_control() const noexcept;
    [[nodiscard]] std::uint32_t effective_interval_us() const noexcept;
    [[nodiscard]] SwapchainLane* lane_for(VkSwapchainKHR swapchain, bool create) noexcept;
    [[nodiscard]] bool ensure_precision_profile(VkDevice device, SwapchainLane* lane) noexcept;
    void consume_feedback_locked(std::uint64_t active_epoch) noexcept;
    void request_new_epoch() noexcept;
    [[nodiscard]] std::uint64_t pace_frame(std::uint64_t requested_frame_id, bool core_origin) noexcept;
    void publish_feedback(
        std::uint64_t frame_id, std::uint64_t timestamp_ns, std::uint64_t epoch,
        std::uint32_t completion_count = 1) noexcept;
    bool init_vkd3d_bridge(IUnknown* pDevice) noexcept;
    bool init_dxvk_bridge(IUnknown* pDevice) noexcept;
    bool init_internal(IUnknown* pDevice, Role role);
    void bind_vkd3d_queue(ID3D12CommandQueue* queue, bool authoritative) noexcept;
    void publish_bridge_present(std::uint64_t frame_id) noexcept;
    void resolve_precision_feedback(VkDevice device, SwapchainLane* lane) noexcept;
    void update_queue_pressure(SwapchainLane* lane) noexcept;
    [[nodiscard]] StructuralEvidenceBatch consume_structural_stall_evidence(
        std::uint64_t completion_timestamp_ns, std::uint64_t active_epoch) noexcept;
    void queue_precision_feedback(
        SwapchainLane* lane, std::uint64_t present_id, std::uint64_t frame_id,
        std::uint64_t frame_epoch, std::uint64_t fallback_timestamp_ns,
        bool uses_present_id2, bool timing_requested) noexcept;

public:
    VulkanFlex();

    bool init(IUnknown* pDevice) override;
    bool init_observer(IUnknown* pDevice);
    bool init_using_ctx(void* context) override;
    void deinit() override;

    Mode get_mode() override { return Mode::VulkanFlex; }
    void* get_tech_context() override { return &ctx_; }
    void set_fg_type(bool, std::uint64_t) override {}
    void set_low_latency_override(ForceReflex value) override;
    void set_effective_fg_state(bool value) override { effective_fg_.store(value, std::memory_order_release); }

    bool is_enabled() override;
    void get_sleep_status(SleepParams* sleep_params) override;
    void set_sleep_mode(SleepMode* sleep_mode) override;
    void sleep() override;
    void sleep_with_frame_id(std::uint64_t frame_id) override;
    void set_marker(IUnknown* pDevice, MarkerParams* marker_params) override;
    void set_async_marker(MarkerParams* marker_params) override;
    void set_async_marker_on_queue(ID3D12CommandQueue* queue, MarkerParams* marker_params) override;
    void observe_d3d12_queue(ID3D12CommandQueue* queue) override;
    void observe_canonical_frame_start(uint64_t frame_id) override;
    void observe_canonical_marker(const MarkerParams* marker) override;
    void observe_canonical_presentation(const PresentationParams* presentation) override;
    void observe_structural_stall(
        StructuralStallKind kind, std::uint64_t duration_ns,
        std::uint64_t end_timestamp_ns) noexcept;

    // Vulkan WSI bridge. Acquire runs after a successful real acquire. Present
    // is deliberately two-phase: begin_present() runs immediately before the
    // real vkQueuePresentKHR, complete_present() only after successful return.
    void on_acquire(VkDevice device, VkSwapchainKHR swapchain, std::uint32_t image_index) noexcept;
    [[nodiscard]] PresentTicket begin_present(
        VkSwapchainKHR swapchain, std::uint32_t image_index) noexcept;
    void complete_present(
        VkDevice device, const PresentTicket& ticket,
        std::uint64_t present_id, bool uses_present_id2, bool timing_requested) noexcept;
    void on_destroy_swapchain(VkSwapchainKHR swapchain) noexcept;
};
