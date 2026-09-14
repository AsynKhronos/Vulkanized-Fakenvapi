#include "ll_vulkanflex.h"

#include "../config.h"
#include "../util.h"
#include "../vulkan_device_registry.h"
#include "../vkd3d_vulkan_capabilities.h"
#include "../transport_truth.h"

#include <algorithm>
#include <limits>

namespace {
std::atomic<std::uint64_t> g_vulkanflex_instance_sequence{1};
constexpr std::uint64_t kControlEnabled = 1ull << 0;
constexpr unsigned kControlIntervalShift = 32;
constexpr std::uint8_t kPrecisionUsesId2 = 1u << 0;
constexpr std::uint8_t kPrecisionTimingRequested = 1u << 1;
constexpr std::uint64_t kPrecisionSlotPublishing = UINT64_MAX;
constexpr std::uint8_t kPrecisionProfileUninitialized = 0;
constexpr std::uint8_t kPrecisionProfilePublishing = 1;
constexpr std::uint8_t kPrecisionProfileReady = 2;
constexpr std::uint8_t kPrecisionProfileUnavailable = 3;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "VulkanFlex requires lock-free 64-bit atomics on the x86-64 target");
static_assert(std::atomic<VkSwapchainKHR>::is_always_lock_free,
              "VulkanFlex requires lock-free Vulkan swapchain handles on the x86-64 target");
}

VulkanFlex::VulkanFlex()
    : instance_id_(g_vulkanflex_instance_sequence.fetch_add(1, std::memory_order_relaxed)) {}

bool VulkanFlex::enabled_from_control() const noexcept {
    const auto force = static_cast<ForceReflex>(override_state_.load(std::memory_order_acquire));
    if (force == ForceReflex::ForceDisable) return false;
    if (force == ForceReflex::ForceEnable) return true;
    return (control_word_.load(std::memory_order_acquire) & kControlEnabled) != 0;
}

std::uint32_t VulkanFlex::effective_interval_us() const noexcept {
    const auto packed = control_word_.load(std::memory_order_acquire);
    const auto frontend = static_cast<std::uint32_t>(packed >> kControlIntervalShift);
    return std::max(frontend, configured_minimum_interval_us_);
}

bool VulkanFlex::init_vkd3d_bridge(IUnknown* pDevice) noexcept {
    if (!pDevice) return false;

    ID3D12Device* d3d12 = nullptr;
    if (FAILED(pDevice->QueryInterface(
            __uuidof(ID3D12Device), reinterpret_cast<void**>(&d3d12))) || !d3d12) {
        return false;
    }

    vkd3d_interop::ID3D12DXVKInteropDeviceMinimal* interop = nullptr;
    const HRESULT interop_hr = d3d12->QueryInterface(
        vkd3d_interop::IID_ID3D12DXVKInteropDevice_Value,
        reinterpret_cast<void**>(&interop));
    if (FAILED(interop_hr) || !interop) {
        d3d12->Release();
        return false;
    }

    Vkd3dVulkanCapabilities caps{};
    const bool probed = probe_vkd3d_vulkan_capabilities(d3d12, &caps);
    d3d12->Release();
    if (!probed || !caps.interop || caps.device == VK_NULL_HANDLE) {
        interop->Release();
        return false;
    }

    vkd3d_interop_ = interop;
    vkd3d_instance_ = caps.instance;
    vkd3d_physical_device_ = caps.physical_device;
    vkd3d_device_ = caps.device;
    vkd3d_interop_v2_ = caps.interop_v2;
    transport_capabilities_ = caps.capabilities;
    transport_ = Transport::Vkd3dD3D12;
    vkd3d_d3d_queue_.store(nullptr, std::memory_order_relaxed);
    vkd3d_vk_queue_.store(VK_NULL_HANDLE, std::memory_order_relaxed);
    vkd3d_queue_family_.store(0xffffffffu, std::memory_order_relaxed);
    last_bridge_feedback_frame_.store(INVALID_ID, std::memory_order_relaxed);
    transport_truth::TransportTruth::confirm_vkd3d();

    spdlog::info(
        "VulkanFlex VKD3D bridge: interop base=true, device2={}, VkDevice={}, Vulkan={}.{} vendor=0x{:04x} device=0x{:04x}",
        vkd3d_interop_v2_, static_cast<const void*>(vkd3d_device_),
        VK_API_VERSION_MAJOR(caps.api_version), VK_API_VERSION_MINOR(caps.api_version),
        caps.vendor_id, caps.device_id);
    spdlog::info(
        "VulkanFlex VKD3D capabilities: NV_low_latency2={}, AMD_anti_lag={}/{}, timeline={}, sync2={}, present_id={}/{}, present_wait={}/{}, present_wait2={}/{}, present_timing={}/{}, descriptor_buffer={}, calibrated_timestamps={}",
        caps.nv_low_latency2_extension, caps.amd_anti_lag_extension, caps.amd_anti_lag_feature,
        caps.timeline_semaphore, caps.synchronization2,
        caps.present_id, caps.present_id_feature, caps.present_wait, caps.present_wait_feature,
        caps.present_wait2, caps.present_wait2_feature, caps.present_timing, caps.present_timing_feature,
        caps.descriptor_buffer, caps.calibrated_timestamps);
    spdlog::info(
        "VulkanFlex VKD3D capability model: vk13={}, vk14={}, EDS1={}/{}, EDS2={}/{}, EDS2.logicOp={}/{}, EDS2.patchCP={}/{}, EDS3={}/{} bits={}/{}, dynamic_rendering={}/{}, GPL={}/{}, shader_module_id={}/{}, unified_layouts={}/{}",
        caps.capabilities.supports(VulkanCapability::Vulkan13),
        caps.capabilities.supports(VulkanCapability::Vulkan14),
        caps.capabilities.supports(VulkanCapability::ExtendedDynamicState1),
        caps.capabilities.is_enabled(VulkanCapability::ExtendedDynamicState1),
        caps.capabilities.supports(VulkanCapability::ExtendedDynamicState2),
        caps.capabilities.is_enabled(VulkanCapability::ExtendedDynamicState2),
        caps.capabilities.supports(VulkanCapability::ExtendedDynamicState2LogicOp),
        caps.capabilities.is_enabled(VulkanCapability::ExtendedDynamicState2LogicOp),
        caps.capabilities.supports(VulkanCapability::ExtendedDynamicState2PatchControlPoints),
        caps.capabilities.is_enabled(VulkanCapability::ExtendedDynamicState2PatchControlPoints),
        caps.capabilities.supports(VulkanCapability::ExtendedDynamicState3),
        caps.capabilities.is_enabled(VulkanCapability::ExtendedDynamicState3),
        vulkan_eds3_feature_count(caps.capabilities.eds3_supported),
        vulkan_eds3_feature_count(caps.capabilities.eds3_enabled),
        caps.capabilities.supports(VulkanCapability::DynamicRendering),
        caps.capabilities.is_enabled(VulkanCapability::DynamicRendering),
        caps.capabilities.supports(VulkanCapability::GraphicsPipelineLibrary),
        caps.capabilities.is_enabled(VulkanCapability::GraphicsPipelineLibrary),
        caps.capabilities.supports(VulkanCapability::ShaderModuleIdentifier),
        caps.capabilities.is_enabled(VulkanCapability::ShaderModuleIdentifier),
        caps.capabilities.supports(VulkanCapability::UnifiedImageLayouts),
        caps.capabilities.is_enabled(VulkanCapability::UnifiedImageLayouts));
    return true;
}

bool VulkanFlex::init_dxvk_bridge(IUnknown* pDevice) noexcept {
    if (!pDevice) return false;

    dxvk_interop::DxvkVulkanCapabilities caps{};
    if (!dxvk_interop::probe_dxvk_vulkan_capabilities(pDevice, &caps) ||
        !caps.interop || caps.device == VK_NULL_HANDLE) {
        return false;
    }

    dxvk_interop::IDXGIVkInteropDeviceMinimal* interop = nullptr;
    if (FAILED(pDevice->QueryInterface(
            dxvk_interop::IID_IDXGIVkInteropDevice_Value,
            reinterpret_cast<void**>(&interop))) || !interop) {
        return false;
    }

    dxvk_interop::ID3DLowLatencyDeviceMinimal* low_latency = nullptr;
    const HRESULT ll_hr = pDevice->QueryInterface(
        dxvk_interop::IID_ID3DLowLatencyDevice_Value,
        reinterpret_cast<void**>(&low_latency));
    const bool ll_interface = SUCCEEDED(ll_hr) && low_latency;
    const bool ll_supported = ll_interface && low_latency->SupportsLowLatency() != FALSE;

    dxvk_interop_ = interop;
    dxvk_low_latency_ = low_latency;
    dxvk_instance_ = caps.instance;
    dxvk_physical_device_ = caps.physical_device;
    dxvk_device_ = caps.device;
    dxvk_queue_ = caps.queue;
    dxvk_queue_family_ = caps.queue_family;
    dxvk_low_latency_supported_ = ll_supported;
    transport_capabilities_ = caps.capabilities;
    transport_ = Transport::DxvkD3D11;
    transport_truth::TransportTruth::confirm_dxvk();

    spdlog::info(
        "VulkanFlex DXVK bridge: interop=true, VkDevice={}, VkQueue={}, family={}, low_latency_interface={}, supported={}",
        static_cast<const void*>(dxvk_device_), static_cast<const void*>(dxvk_queue_),
        dxvk_queue_family_, ll_interface, ll_supported);
    if (caps.capabilities.api_version != 0) {
        spdlog::info(
            "VulkanFlex DXVK capability model: api={}.{}, vk13={}, vk14={}, EDS1={}, EDS2={}, EDS2.logicOp={}, EDS2.patchCP={}, EDS3={} bits={}, dynamic_rendering={}, sync2={}, timeline={}, descriptor_buffer={}, GPL={}, shader_module_id={}, unified_layouts={} (enabled-state mostly unknown by public DXVK ABI)",
            VK_API_VERSION_MAJOR(caps.capabilities.api_version),
            VK_API_VERSION_MINOR(caps.capabilities.api_version),
            caps.capabilities.supports(VulkanCapability::Vulkan13),
            caps.capabilities.supports(VulkanCapability::Vulkan14),
            caps.capabilities.supports(VulkanCapability::ExtendedDynamicState1),
            caps.capabilities.supports(VulkanCapability::ExtendedDynamicState2),
            caps.capabilities.supports(VulkanCapability::ExtendedDynamicState2LogicOp),
            caps.capabilities.supports(VulkanCapability::ExtendedDynamicState2PatchControlPoints),
            caps.capabilities.supports(VulkanCapability::ExtendedDynamicState3),
            vulkan_eds3_feature_count(caps.capabilities.eds3_supported),
            caps.capabilities.supports(VulkanCapability::DynamicRendering),
            caps.capabilities.supports(VulkanCapability::Synchronization2),
            caps.capabilities.supports(VulkanCapability::TimelineSemaphore),
            caps.capabilities.supports(VulkanCapability::DescriptorBuffer),
            caps.capabilities.supports(VulkanCapability::GraphicsPipelineLibrary),
            caps.capabilities.supports(VulkanCapability::ShaderModuleIdentifier),
            caps.capabilities.supports(VulkanCapability::UnifiedImageLayouts));
    }
    return true;
}

bool VulkanFlex::init(IUnknown* pDevice) {
    return init_internal(pDevice, Role::Executor);
}

bool VulkanFlex::init_observer(IUnknown* pDevice) {
    return init_internal(pDevice, Role::Observer);
}

bool VulkanFlex::init_internal(IUnknown* pDevice, Role role) {
    const auto& policy = Config::get().snapshot().vulkanflex;
    if (!policy.enabled)
        return false;

    role_ = role;
    transport_ = Transport::NativeVulkan;
    transport_capabilities_ = {};
    native_wsi_authoritative_ = false;
    if (pDevice) {
        if (!(policy.vkd3d_bridge && init_vkd3d_bridge(pDevice)) &&
            !(policy.dxvk_bridge && init_dxvk_bridge(pDevice))) {
            return false;
        }

        if (transport_ == Transport::DxvkD3D11 && role_ == Role::Executor) {
            if (dxvk_low_latency_supported_) {
                // DXVK owns the actual wait. VulkanFlex remains the orchestration
                // backend so canonical FrameTokens and fused controls reach it.
                role_ = Role::Delegate;
            } else if (policy.dxvk_execution != policy::AutoBool::Enabled) {
                // Auto must never create a second independent scheduler. Drop
                // bridge references before reporting an unusable executor.
                if (dxvk_low_latency_) {
                    dxvk_low_latency_->Release();
                    dxvk_low_latency_ = nullptr;
                }
                if (dxvk_interop_) {
                    dxvk_interop_->Release();
                    dxvk_interop_ = nullptr;
                }
                dxvk_low_latency_supported_ = false;
                transport_ = Transport::NativeVulkan;
                return false;
            }
        }
    } else if (role_ == Role::Observer) {
        // Observer mode exists specifically for translation stacks and must
        // never create a second native-Vulkan execution plane.
        return false;
    } else {
        const auto access = transport_truth::TransportTruth::native_execution_access();
        if (access == transport_truth::NativeExecutionAccess::Blocked) {
            spdlog::warn(
                "VulkanFlex native executor rejected: authoritative transport={} chain={}",
                transport_truth::to_string(transport_truth::TransportTruth::transport()),
                transport_truth::to_string(transport_truth::TransportTruth::vulkan_chain_state()));
            return false;
        }
        native_wsi_authoritative_ = access == transport_truth::NativeExecutionAccess::FullWsi;
        if (!native_wsi_authoritative_) {
            const auto truth = transport_truth::TransportTruth::snapshot();
            spdlog::warn(
                "VulkanFlex native executor restricted to explicit frontend pacing: transport={} chain={} evidence=0x{:02x}; core WSI/present precision/queue pressure/structural stall disabled",
                transport_truth::to_string(truth.transport),
                transport_truth::to_string(truth.vulkan_chain),
                truth.evidence);
        }
    }

    // Core WSI pacing only exists on an authoritatively intercepted native
    // Vulkan executor. A weak translation-layer bypass may still permit
    // explicit marker/sleep pacing, but never WSI-derived features. VKD3D bridge
    // mode is driven by D3D12 semantics, and observer mode never sleeps.
    core_pacing_enabled_ = role_ == Role::Executor && policy.core_pacing &&
        transport_ == Transport::NativeVulkan && native_wsi_authoritative_;
    configured_minimum_interval_us_ = policy.minimum_interval_us;
    max_sleep_us_ = policy.max_sleep_us;
    present_precision_enabled_ = policy.present_precision != policy::AutoBool::Disabled &&
        role_ == Role::Executor && transport_ == Transport::NativeVulkan && native_wsi_authoritative_;
    queue_pressure_mode_ = policy.queue_pressure == policy::AutoBool::Disabled
        ? QueuePressureMode::Disabled
        : (policy.queue_pressure == policy::AutoBool::Enabled ? QueuePressureMode::Enabled : QueuePressureMode::Auto);
    queue_target_presents_ = policy.queue_target_presents;
    queue_max_delay_us_ = policy.queue_max_delay_us;
    queue_pressure_enabled_ = role_ == Role::Executor &&
        transport_ == Transport::NativeVulkan && native_wsi_authoritative_ && present_precision_enabled_ &&
        queue_pressure_mode_ != QueuePressureMode::Disabled && queue_max_delay_us_ != 0;
    structural_stall_mode_ = policy.structural_stall == policy::AutoBool::Disabled
        ? StructuralStallMode::Disabled
        : (policy.structural_stall == policy::AutoBool::Enabled
            ? StructuralStallMode::Enabled : StructuralStallMode::Auto);
    pipeline_threshold_ns_ = static_cast<std::uint64_t>(policy.pipeline_threshold_us) * 1000ull;
    stall_max_compensation_ns_ = static_cast<std::uint64_t>(policy.stall_max_compensation_us) * 1000ull;
    structural_stall_enabled_ = role_ == Role::Executor &&
        transport_ == Transport::NativeVulkan && native_wsi_authoritative_ &&
        structural_stall_mode_ != StructuralStallMode::Disabled &&
        pipeline_threshold_ns_ != 0 && stall_max_compensation_ns_ != 0;

    control_word_.store(role_ != Role::Observer ? kControlEnabled : 0, std::memory_order_release);
    frame_sequence_.store(0, std::memory_order_relaxed);
    last_started_frame_id_.store(0, std::memory_order_relaxed);
    epoch_.store(1, std::memory_order_relaxed);
    reset_requested_.store(false, std::memory_order_relaxed);
    primary_swapchain_.store(VK_NULL_HANDLE, std::memory_order_relaxed);
    explicit_pacing_.store(static_cast<std::uint8_t>(ExplicitPacing::None), std::memory_order_relaxed);
    core_paced_pending_.store(false, std::memory_order_relaxed);
    dropped_feedback_.store(0, std::memory_order_relaxed);
    rejected_feedback_.store(0, std::memory_order_relaxed);
    precision_timing_hits_.store(0, std::memory_order_relaxed);
    precision_wait2_hits_.store(0, std::memory_order_relaxed);
    precision_wait_hits_.store(0, std::memory_order_relaxed);
    precision_fallbacks_.store(0, std::memory_order_relaxed);
    queue_pressure_delay_ns_.store(0, std::memory_order_relaxed);
    queue_started_frames_.store(0, std::memory_order_relaxed);
    queue_completed_frames_.store(0, std::memory_order_relaxed);
    queue_observed_frame_time_ns_.store(0, std::memory_order_relaxed);
    queue_pressure_events_.store(0, std::memory_order_relaxed);
    queue_pressure_total_delay_ns_.store(0, std::memory_order_relaxed);
    queue_pressure_max_pending_.store(0, std::memory_order_relaxed);
    queue_pressure_trust_logged_.store(false, std::memory_order_relaxed);
    structural_stall_cursor_.store(0, std::memory_order_relaxed);
    structural_last_completion_ns_.store(0, std::memory_order_relaxed);
    structural_last_completion_frame_.store(0, std::memory_order_relaxed);
    structural_baseline_ns_.store(0, std::memory_order_relaxed);
    structural_baseline_samples_.store(0, std::memory_order_relaxed);
    structural_pipeline_events_.store(0, std::memory_order_relaxed);
    structural_compensated_events_.store(0, std::memory_order_relaxed);
    structural_compensated_ns_.store(0, std::memory_order_relaxed);
    structural_dropped_events_.store(0, std::memory_order_relaxed);
    structural_trust_logged_.store(false, std::memory_order_relaxed);
    for (auto& slot : structural_stall_slots_) {
        slot.end_timestamp_ns.store(0, std::memory_order_relaxed);
        slot.duration_ns.store(0, std::memory_order_relaxed);
        slot.epoch.store(0, std::memory_order_relaxed);
        slot.kind.store(static_cast<std::uint8_t>(StructuralStallKind::GraphicsPipeline), std::memory_order_relaxed);
    }
    observer_frame_count_.store(0, std::memory_order_relaxed);
    observer_present_count_.store(0, std::memory_order_relaxed);
    observer_presentation_count_.store(0, std::memory_order_relaxed);
    observer_generated_present_count_.store(0, std::memory_order_relaxed);
    observer_last_frame_.store(0, std::memory_order_relaxed);
    observer_last_presentation_.store(0, std::memory_order_relaxed);
    observer_last_present_ns_.store(0, std::memory_order_relaxed);
    feedback_.reset();
    ctx_.Reset();
    ctx_.target_frame_time = static_cast<std::uint64_t>(configured_minimum_interval_us_) * 1000ull;
    spdlog::info(
        "VulkanFlex initialized: role={}, transport={}, core_pacing={}, present_precision={}, queue_pressure={}, queue_target={}, queue_max_delay_us={}, structural_stall={}, pipeline_threshold_us={}, stall_max_compensation_us={}, min_interval_us={}, max_sleep_us={}, lanes={}x{}",
        role_ == Role::Observer ? "observer" : (role_ == Role::Delegate ? "delegate" : "executor"),
        transport_ == Transport::Vkd3dD3D12 ? "vkd3d-d3d12" :
            (transport_ == Transport::DxvkD3D11 ? "dxvk-d3d11" :
                (native_wsi_authoritative_ ? "native-vulkan" : "native-vulkan-explicit")),
        core_pacing_enabled_, present_precision_enabled_, queue_pressure_enabled_,
        queue_target_presents_, queue_max_delay_us_, structural_stall_enabled_,
        pipeline_threshold_ns_ / 1000ull, stall_max_compensation_ns_ / 1000ull,
        configured_minimum_interval_us_, max_sleep_us_, kSwapchainLanes, kImagesPerLane);
    if (transport_ == Transport::DxvkD3D11) {
        spdlog::info(
            "VulkanFlex DXVK pacing ownership: owner={}, role={}, native_ll_supported={}",
            role_ == Role::Delegate ? "dxvk" : (role_ == Role::Executor ? "vulkanflex" : "external/none"),
            role_ == Role::Delegate ? "delegate" : (role_ == Role::Executor ? "executor" : "observer"),
            dxvk_low_latency_supported_);
    }
    return true;
}

bool VulkanFlex::init_using_ctx(void*) {
    inited_using_context = false;
    return false;
}

void VulkanFlex::deinit() {
    // Backend lifetime is serialized by LowLatency.  Clear publication-visible
    // state so stale WSI callbacks fail open rather than pacing a new epoch.
    control_word_.store(0, std::memory_order_release);
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    reset_requested_.store(false, std::memory_order_release);
    primary_swapchain_.store(VK_NULL_HANDLE, std::memory_order_release);
    explicit_pacing_.store(static_cast<std::uint8_t>(ExplicitPacing::None), std::memory_order_release);
    for (auto& lane : swapchains_) {
        lane.swapchain.store(VK_NULL_HANDLE, std::memory_order_release);
        for (std::size_t i = 0; i < kImagesPerLane; ++i) {
            lane.frame_ids[i].store(0, std::memory_order_relaxed);
            lane.frame_epochs[i].store(0, std::memory_order_relaxed);
        }
        for (auto& pending : lane.precision_pending) {
            pending.present_id.store(0, std::memory_order_relaxed);
            pending.frame_id.store(0, std::memory_order_relaxed);
            pending.epoch.store(0, std::memory_order_relaxed);
            pending.fallback_timestamp_ns.store(0, std::memory_order_relaxed);
            pending.flags.store(0, std::memory_order_relaxed);
            pending.polls.store(0, std::memory_order_relaxed);
        }
        lane.precision_pending_count.store(0, std::memory_order_relaxed);
        lane.precision_probe_cursor.store(0, std::memory_order_relaxed);
        lane.precision_profile_state.store(kPrecisionProfileUninitialized, std::memory_order_relaxed);
        lane.precision_device = VK_NULL_HANDLE;
        lane.precision_swapchain_flags = 0;
        lane.precision_dispatch = {};
        lane.best_precision_tier.store(0, std::memory_order_relaxed);
        lane.precise_completions.store(0, std::memory_order_relaxed);
        lane.fallback_completions.store(0, std::memory_order_relaxed);
        lane.max_pending_seen.store(0, std::memory_order_relaxed);
        lane.pressure_epoch.store(0, std::memory_order_relaxed);
    }
    feedback_.reset();
    for (auto& slot : structural_stall_slots_) {
        slot.end_timestamp_ns.store(0, std::memory_order_release);
        slot.duration_ns.store(0, std::memory_order_relaxed);
        slot.epoch.store(0, std::memory_order_relaxed);
    }
    structural_last_completion_ns_.store(0, std::memory_order_relaxed);
    structural_last_completion_frame_.store(0, std::memory_order_relaxed);
    structural_baseline_ns_.store(0, std::memory_order_relaxed);
    structural_baseline_samples_.store(0, std::memory_order_relaxed);
    structural_trust_logged_.store(false, std::memory_order_relaxed);
    vkd3d_d3d_queue_.store(nullptr, std::memory_order_release);
    vkd3d_vk_queue_.store(VK_NULL_HANDLE, std::memory_order_release);
    vkd3d_queue_family_.store(0xffffffffu, std::memory_order_release);
    last_bridge_feedback_frame_.store(INVALID_ID, std::memory_order_release);
    if (vkd3d_interop_) {
        vkd3d_interop_->Release();
        vkd3d_interop_ = nullptr;
    }
    vkd3d_instance_ = VK_NULL_HANDLE;
    vkd3d_physical_device_ = VK_NULL_HANDLE;
    vkd3d_device_ = VK_NULL_HANDLE;
    vkd3d_interop_v2_ = false;
    if (dxvk_low_latency_) {
        dxvk_low_latency_->Release();
        dxvk_low_latency_ = nullptr;
    }
    if (dxvk_interop_) {
        dxvk_interop_->Release();
        dxvk_interop_ = nullptr;
    }
    dxvk_instance_ = VK_NULL_HANDLE;
    dxvk_physical_device_ = VK_NULL_HANDLE;
    dxvk_device_ = VK_NULL_HANDLE;
    dxvk_queue_ = VK_NULL_HANDLE;
    dxvk_queue_family_ = 0xffffffffu;
    dxvk_low_latency_supported_ = false;
    transport_capabilities_ = {};
    transport_ = Transport::NativeVulkan;
    const auto role = role_;
    role_ = Role::Executor;
    const auto dropped = dropped_feedback_.load(std::memory_order_relaxed);
    if (role == Role::Observer) {
        spdlog::info(
            "VulkanFlex observer deinitialized: render_frames={}, present_markers={}, fg_presentations={}, generated={}, last_render={}, last_presentation={}",
            observer_frame_count_.load(std::memory_order_relaxed),
            observer_present_count_.load(std::memory_order_relaxed),
            observer_presentation_count_.load(std::memory_order_relaxed),
            observer_generated_present_count_.load(std::memory_order_relaxed),
            observer_last_frame_.load(std::memory_order_relaxed),
            observer_last_presentation_.load(std::memory_order_relaxed));
    } else {
        spdlog::info(
            "VulkanFlex deinitialized: precision timing={}, wait2={}, wait={}, fallback={}, queue_events={}, queue_delay_us={}, queue_max_pending={}, structural_pipeline_events={}, structural_compensated={}, structural_comp_us={}, structural_dropped={}, dropped_feedback={}, rejected_feedback={}",
            precision_timing_hits_.load(std::memory_order_relaxed),
            precision_wait2_hits_.load(std::memory_order_relaxed),
            precision_wait_hits_.load(std::memory_order_relaxed),
            precision_fallbacks_.load(std::memory_order_relaxed),
            queue_pressure_events_.load(std::memory_order_relaxed),
            queue_pressure_total_delay_ns_.load(std::memory_order_relaxed) / 1000ull,
            queue_pressure_max_pending_.load(std::memory_order_relaxed),
            structural_pipeline_events_.load(std::memory_order_relaxed),
            structural_compensated_events_.load(std::memory_order_relaxed),
            structural_compensated_ns_.load(std::memory_order_relaxed) / 1000ull,
            structural_dropped_events_.load(std::memory_order_relaxed), dropped,
            rejected_feedback_.load(std::memory_order_relaxed));
    }
}

void VulkanFlex::request_new_epoch() noexcept {
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    reset_requested_.store(true, std::memory_order_release);
    explicit_pacing_.store(static_cast<std::uint8_t>(ExplicitPacing::None), std::memory_order_release);
    core_paced_pending_.store(false, std::memory_order_release);
    queue_pressure_delay_ns_.store(0, std::memory_order_release);
    queue_started_frames_.store(0, std::memory_order_relaxed);
    queue_completed_frames_.store(0, std::memory_order_relaxed);
    queue_observed_frame_time_ns_.store(0, std::memory_order_relaxed);
    queue_pressure_trust_logged_.store(false, std::memory_order_relaxed);
    structural_last_completion_ns_.store(0, std::memory_order_relaxed);
    structural_last_completion_frame_.store(0, std::memory_order_relaxed);
    structural_baseline_ns_.store(0, std::memory_order_relaxed);
    structural_baseline_samples_.store(0, std::memory_order_relaxed);
    structural_trust_logged_.store(false, std::memory_order_relaxed);
    for (auto& slot : structural_stall_slots_)
        slot.end_timestamp_ns.store(0, std::memory_order_release);
    last_started_frame_id_.store(0, std::memory_order_release);
}

void VulkanFlex::set_low_latency_override(ForceReflex value) {
    const bool was_enabled = enabled_from_control();
    override_state_.store(static_cast<std::uint8_t>(value), std::memory_order_release);
    const bool now_enabled = enabled_from_control();
    if (was_enabled != now_enabled)
        request_new_epoch();
}

bool VulkanFlex::is_enabled() {
    return role_ != Role::Observer && enabled_from_control();
}

void VulkanFlex::get_sleep_status(SleepParams* sleep_params) {
    if (!sleep_params) return;
    sleep_params->low_latency_enabled = enabled_from_control();
    sleep_params->fullscreen_vrr = true;
    sleep_params->control_panel_vsync_override = false;
}

void VulkanFlex::set_sleep_mode(SleepMode* sleep_mode) {
    if (!sleep_mode || role_ == Role::Observer) return;
    const bool was_enabled = enabled_from_control();
    std::uint64_t packed = sleep_mode->low_latency_enabled ? kControlEnabled : 0;
    packed |= static_cast<std::uint64_t>(sleep_mode->minimum_interval_us) << kControlIntervalShift;
    control_word_.store(packed, std::memory_order_release);
    const bool now_enabled = enabled_from_control();
    if (was_enabled != now_enabled)
        request_new_epoch();

    if (role_ == Role::Delegate && dxvk_low_latency_) {
        const HRESULT hr = dxvk_low_latency_->SetLatencySleepMode(
            sleep_mode->low_latency_enabled ? TRUE : FALSE,
            sleep_mode->low_latency_boost ? TRUE : FALSE,
            std::max(sleep_mode->minimum_interval_us, configured_minimum_interval_us_));
        if (FAILED(hr))
            spdlog::debug("VulkanFlex DXVK delegated SetLatencySleepMode failed: hr=0x{:08x}",
                          static_cast<unsigned>(hr));
    }
}

VulkanFlex::SwapchainLane* VulkanFlex::lane_for(VkSwapchainKHR swapchain, bool create) noexcept {
    if (swapchain == VK_NULL_HANDLE) return nullptr;

    struct TlsLane {
        const VulkanFlex* owner = nullptr;
        std::uint64_t owner_id = 0;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        SwapchainLane* lane = nullptr;
    };
    static thread_local TlsLane tls{};

    if (tls.owner == this && tls.owner_id == instance_id_ &&
        tls.swapchain == swapchain && tls.lane &&
        tls.lane->swapchain.load(std::memory_order_acquire) == swapchain) {
        return tls.lane;
    }

    for (auto& lane : swapchains_) {
        if (lane.swapchain.load(std::memory_order_acquire) == swapchain) {
            tls = {this, instance_id_, swapchain, &lane};
            return &lane;
        }
    }

    if (!create)
        return nullptr;

    // Creation is rare, while lookup is per-frame. Serialize only the creation
    // slow path so a new lane stays invisible until every field is initialized.
    // If another creator is active, fail open after one bounded rescan instead
    // of making a render thread spin behind it.
    if (lane_create_gate_.test_and_set(std::memory_order_acquire)) {
        for (auto& lane : swapchains_) {
            if (lane.swapchain.load(std::memory_order_acquire) == swapchain) {
                tls = {this, instance_id_, swapchain, &lane};
                return &lane;
            }
        }
        return nullptr;
    }

    SwapchainLane* selected = nullptr;
    for (auto& lane : swapchains_) {
        const auto current = lane.swapchain.load(std::memory_order_acquire);
        if (current == swapchain) {
            lane_create_gate_.clear(std::memory_order_release);
            tls = {this, instance_id_, swapchain, &lane};
            return &lane;
        }
        if (current == VK_NULL_HANDLE && !selected)
            selected = &lane;
    }

    if (!selected) {
        lane_create_gate_.clear(std::memory_order_release);
        return nullptr;
    }

    // Keep swapchain == VK_NULL_HANDLE until initialization is complete. This
    // fixes the old publication window where another thread could observe the
    // identity before stale per-image/per-precision state had been cleared.
    for (std::size_t i = 0; i < kImagesPerLane; ++i) {
        selected->frame_ids[i].store(0, std::memory_order_relaxed);
        selected->frame_epochs[i].store(0, std::memory_order_relaxed);
    }
    for (auto& pending : selected->precision_pending) {
        pending.present_id.store(0, std::memory_order_relaxed);
        pending.frame_id.store(0, std::memory_order_relaxed);
        pending.epoch.store(0, std::memory_order_relaxed);
        pending.fallback_timestamp_ns.store(0, std::memory_order_relaxed);
        pending.flags.store(0, std::memory_order_relaxed);
        pending.polls.store(0, std::memory_order_relaxed);
    }
    selected->precision_pending_count.store(0, std::memory_order_relaxed);
    selected->precision_probe_cursor.store(0, std::memory_order_relaxed);
    selected->precision_profile_state.store(kPrecisionProfileUninitialized, std::memory_order_relaxed);
    selected->precision_device = VK_NULL_HANDLE;
    selected->precision_swapchain_flags = 0;
    selected->precision_dispatch = {};
    selected->best_precision_tier.store(0, std::memory_order_relaxed);
    selected->precise_completions.store(0, std::memory_order_relaxed);
    selected->fallback_completions.store(0, std::memory_order_relaxed);
    selected->max_pending_seen.store(0, std::memory_order_relaxed);
    selected->pressure_epoch.store(epoch_.load(std::memory_order_relaxed), std::memory_order_relaxed);

    selected->swapchain.store(swapchain, std::memory_order_release);
    lane_create_gate_.clear(std::memory_order_release);
    tls = {this, instance_id_, swapchain, selected};
    return selected;
}

bool VulkanFlex::ensure_precision_profile(VkDevice device, SwapchainLane* lane) noexcept {
    if (!present_precision_enabled_ || device == VK_NULL_HANDLE || !lane)
        return false;

    auto state = lane->precision_profile_state.load(std::memory_order_acquire);
    if (state == kPrecisionProfileReady)
        return lane->precision_device == device;
    if (state == kPrecisionProfileUnavailable || state == kPrecisionProfilePublishing)
        return false;

    auto expected = kPrecisionProfileUninitialized;
    if (!lane->precision_profile_state.compare_exchange_strong(
            expected, kPrecisionProfilePublishing,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return expected == kPrecisionProfileReady && lane->precision_device == device;
    }

    const auto swapchain = lane->swapchain.load(std::memory_order_acquire);
    VulkanPresentPrecisionDispatch dispatch{};
    VulkanSwapchainState swapchain_state{};
    const bool usable = swapchain != VK_NULL_HANDLE &&
        VulkanDeviceRegistry::get_present_precision_dispatch(device, &dispatch) &&
        VulkanDeviceRegistry::get_swapchain_state(swapchain, &swapchain_state) &&
        swapchain_state.device == device;

    if (usable) {
        lane->precision_device = device;
        lane->precision_swapchain_flags = swapchain_state.flags;
        lane->precision_dispatch = dispatch;
        lane->precision_profile_state.store(kPrecisionProfileReady, std::memory_order_release);
        return true;
    }

    lane->precision_device = VK_NULL_HANDLE;
    lane->precision_swapchain_flags = 0;
    lane->precision_dispatch = {};
    lane->precision_profile_state.store(kPrecisionProfileUnavailable, std::memory_order_release);
    return false;
}

void VulkanFlex::publish_feedback(
    std::uint64_t frame_id, std::uint64_t timestamp_ns, std::uint64_t epoch,
    std::uint32_t completion_count) noexcept {
    if (frame_id == 0 || timestamp_ns == 0 || epoch == 0 || completion_count == 0)
        return;

    // Old-epoch presentation callbacks can race an Off->On/reset transition.
    // They are not useful to the new scheduler epoch and must not inflate its
    // CPU-ahead accounting.
    if (epoch != epoch_.load(std::memory_order_relaxed))
        return;

    // Queue-depth accounting describes real retired work, not whether the
    // estimator feedback ring happened to have room. A burst may be coalesced
    // into one estimator sample while still retiring every completed present.
    queue_completed_frames_.fetch_add(completion_count, std::memory_order_relaxed);
    if (!feedback_.try_enqueue(Feedback{frame_id, timestamp_ns, epoch}))
        dropped_feedback_.fetch_add(1, std::memory_order_relaxed);
}

VulkanFlex::StructuralEvidenceBatch VulkanFlex::consume_structural_stall_evidence(
    std::uint64_t completion_timestamp_ns, std::uint64_t active_epoch) noexcept {
    StructuralEvidenceBatch batch{};
    std::uint64_t strongest_duration = 0;
    constexpr std::uint64_t kRecentWindowNs = 100'000'000ull;

    for (auto& slot : structural_stall_slots_) {
        const auto end_ns = slot.end_timestamp_ns.load(std::memory_order_acquire);
        if (end_ns == 0 || end_ns == kStallSlotPublishing || end_ns > completion_timestamp_ns)
            continue;

        const auto duration_ns = slot.duration_ns.load(std::memory_order_relaxed);
        const auto event_epoch = slot.epoch.load(std::memory_order_relaxed);
        const auto kind = static_cast<StructuralStallKind>(slot.kind.load(std::memory_order_relaxed));

        auto expected = end_ns;
        if (!slot.end_timestamp_ns.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
            continue;
        }

        if (event_epoch != active_epoch || completion_timestamp_ns - end_ns > kRecentWindowNs)
            continue;

        ++batch.events;
        if (duration_ns > strongest_duration ||
            (duration_ns == strongest_duration && end_ns > batch.latest_end_ns)) {
            strongest_duration = duration_ns;
            batch.duration_ns = duration_ns;
            batch.latest_end_ns = end_ns;
            batch.strongest_kind = kind;
        }
    }
    return batch;
}

void VulkanFlex::consume_feedback_locked(std::uint64_t active_epoch) noexcept {
    Feedback feedback{};
    std::uint64_t latency = 0;
    std::uint64_t frame_time = 0;
    while (feedback_.try_dequeue(&feedback)) {
        if (feedback.epoch != active_epoch)
            continue;

        const auto previous_ns = structural_last_completion_ns_.load(std::memory_order_relaxed);
        const auto previous_frame = structural_last_completion_frame_.load(std::memory_order_relaxed);
        const bool first_feedback = previous_ns == 0 || previous_frame == 0;
        const bool monotonic = !first_feedback &&
            feedback.timestamp_ns > previous_ns && feedback.frame_id > previous_frame;

        // Completion observations can be published by different WSI/marker
        // producers. Never let an older frame or an older timestamp move the
        // LatencyFleX end-frame state backwards; that corrupts projection and
        // can create a false queue spike on the next frame. Precision batches
        // are coalesced earlier, this is the final defensive ordering gate.
        if (!first_feedback && !monotonic) {
            rejected_feedback_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const auto frames_elapsed = monotonic ? feedback.frame_id - previous_frame : 0;
        const auto baseline_ns = structural_baseline_ns_.load(std::memory_order_relaxed);
        const auto baseline_samples = structural_baseline_samples_.load(std::memory_order_relaxed);

        StructuralEvidenceBatch evidence{};
        StructuralStallDecision stall{};
        if (structural_stall_enabled_) {
            evidence = consume_structural_stall_evidence(feedback.timestamp_ns, active_epoch);
            if (monotonic && baseline_ns != 0) {
                const auto expected_gap = baseline_ns >
                        std::numeric_limits<std::uint64_t>::max() / frames_elapsed
                    ? std::numeric_limits<std::uint64_t>::max()
                    : baseline_ns * frames_elapsed;
                StructuralStallConfig config{};
                config.mode = structural_stall_mode_;
                config.pipeline_threshold_ns = pipeline_threshold_ns_;
                config.max_compensation_ns = stall_max_compensation_ns_;
                StructuralStallSample sample{};
                sample.completion_timestamp_ns = feedback.timestamp_ns;
                sample.previous_completion_timestamp_ns = previous_ns;
                sample.expected_gap_ns = expected_gap;
                sample.baseline_samples = baseline_samples;
                sample.pending_pipeline_duration_ns = evidence.duration_ns;
                sample.pending_pipeline_end_ns = evidence.latest_end_ns;
                stall = classify_structural_stall(config, sample);

                if (stall.trusted &&
                    !structural_trust_logged_.exchange(true, std::memory_order_acq_rel)) {
                    spdlog::info(
                        "VulkanFlex structural-stall classifier armed: baseline_us={}, samples={}, pipeline_threshold_us={}, max_compensation_us={}",
                        baseline_ns / 1000ull, baseline_samples, pipeline_threshold_ns_ / 1000ull,
                        stall_max_compensation_ns_ / 1000ull);
                }
                if (stall.structural) {
                    const auto stall_start_ns = evidence.latest_end_ns > stall.compensation_ns
                        ? evidence.latest_end_ns - stall.compensation_ns : 0;
                    ctx_.CompensateExternalStall(
                        stall.compensation_ns, stall_start_ns, evidence.latest_end_ns);
                    queue_pressure_delay_ns_.store(0, std::memory_order_release);
                    structural_compensated_events_.fetch_add(1, std::memory_order_relaxed);
                    structural_compensated_ns_.fetch_add(stall.compensation_ns, std::memory_order_relaxed);
                    spdlog::debug(
                        "VulkanFlex structural stall shielded: kind={}, frame={}, raw_gap_us={}, expected_us={}, pipeline_us={}, compensation_us={}, events={}",
                        structural_stall_kind_name(evidence.strongest_kind), feedback.frame_id,
                        stall.raw_gap_ns / 1000ull, expected_gap / 1000ull,
                        evidence.duration_ns / 1000ull, stall.compensation_ns / 1000ull, evidence.events);
                }
            }
        }

        frame_time = 0;
        ctx_.EndFrame(feedback.frame_id, feedback.timestamp_ns, &latency, &frame_time);
        if (frame_time != 0 && frame_time != UINT64_MAX)
            queue_observed_frame_time_ns_.store(frame_time, std::memory_order_relaxed);

        if (monotonic) {
            const auto raw_gap = feedback.timestamp_ns - previous_ns;
            const auto effective_gap = stall.structural && stall.compensation_ns < raw_gap
                ? raw_gap - stall.compensation_ns : raw_gap;
            const auto per_frame_ns = effective_gap / frames_elapsed;
            const auto updated = update_structural_baseline_ns(baseline_ns, per_frame_ns);
            if (per_frame_ns >= 500'000ull && per_frame_ns <= 100'000'000ull) {
                structural_baseline_ns_.store(updated, std::memory_order_relaxed);
                if (baseline_samples != UINT32_MAX)
                    structural_baseline_samples_.store(baseline_samples + 1, std::memory_order_relaxed);
            }
        } else if (previous_ns == 0 || previous_frame == 0) {
            // Seed only the completion anchor. A baseline requires an actual gap.
        }

        if ((previous_frame == 0 || feedback.frame_id > previous_frame) &&
            (previous_ns == 0 || feedback.timestamp_ns > previous_ns)) {
            structural_last_completion_ns_.store(feedback.timestamp_ns, std::memory_order_relaxed);
            structural_last_completion_frame_.store(feedback.frame_id, std::memory_order_relaxed);
        }
    }
}

std::uint64_t VulkanFlex::pace_frame(std::uint64_t requested_frame_id, bool core_origin) noexcept {
    if (!enabled_from_control()) return 0;

    // LatencyFleX requires external synchronization. A contended callback must
    // never spin or block a render/simulation thread, so fail open and omit this
    // sample. WSI/marker processing continues normally. The gate also makes the
    // internal frame sequence single-writer, avoiding a second locked RMW.
    if (scheduler_gate_.test_and_set(std::memory_order_acquire))
        return 0;

    std::uint64_t frame_id = requested_frame_id;
    if (frame_id == 0 || frame_id == INVALID_ID) {
        frame_id = frame_sequence_.load(std::memory_order_relaxed) + 1;
        frame_sequence_.store(frame_id, std::memory_order_relaxed);
    }

    // Reset requests are exceptional. Avoid a locked RMW on every paced frame
    // when the flag is false; only the frame that observes a pending reset pays
    // the exchange needed to claim it. scheduler_gate_ serializes this section.
    if (reset_requested_.load(std::memory_order_acquire) &&
        reset_requested_.exchange(false, std::memory_order_acq_rel)) {
        // Drain old producer publications before starting a fresh LatencyFleX
        // epoch. Feedback carries an epoch tag as a second line of defence for
        // presents that race with an Off->On transition.
        Feedback stale{};
        while (feedback_.try_dequeue(&stale)) {}
        ctx_.Reset();
    }
    const auto active_epoch = epoch_.load(std::memory_order_acquire);
    consume_feedback_locked(active_epoch);
    ctx_.target_frame_time = static_cast<std::uint64_t>(effective_interval_us()) * 1000ull;

    const auto now = get_timestamp();
    const auto latencyflex_target = ctx_.GetWaitTarget(frame_id);
    std::uint64_t pressure_delay_ns = 0;
    if (queue_pressure_enabled_) {
        // Zero is overwhelmingly the common governor state. Keep that path a
        // read-only cache-line access and reserve the locked exchange for an
        // actual non-zero delay that must be consumed exactly once.
        pressure_delay_ns = queue_pressure_delay_ns_.load(std::memory_order_acquire);
        if (pressure_delay_ns != 0)
            pressure_delay_ns = queue_pressure_delay_ns_.exchange(0, std::memory_order_acq_rel);
    }
    const auto governor_target = pressure_delay_ns != 0 ? now + pressure_delay_ns : now;
    const auto target = std::max(latencyflex_target, governor_target);
    const auto base_target = std::max(latencyflex_target, now);
    const auto added_delay_ns = target > base_target ? target - base_target : 0;
    if (added_delay_ns != 0) {
        queue_pressure_events_.fetch_add(1, std::memory_order_relaxed);
        queue_pressure_total_delay_ns_.fetch_add(added_delay_ns, std::memory_order_relaxed);
    }
    auto wake_timestamp = now;
    if (target > now) {
        const auto max_sleep_ns = static_cast<std::uint64_t>(max_sleep_us_) * 1000ull;
        const auto sleep_ns = std::min(target - now, max_sleep_ns);
        if (sleep_ns != 0) {
            (void)eepy(sleep_ns);
            // Use the intended target when it was reached; otherwise report the
            // actual capped wake time so LatencyFleX can correct its projection.
            wake_timestamp = sleep_ns == (target - now) ? target : get_timestamp();
        }
    }

    ctx_.BeginFrame(frame_id, target, wake_timestamp);
    queue_started_frames_.fetch_add(1, std::memory_order_relaxed);
    last_started_frame_id_.store(frame_id, std::memory_order_release);
    if (core_origin)
        core_paced_pending_.store(true, std::memory_order_release);

    scheduler_gate_.clear(std::memory_order_release);
    return frame_id;
}

void VulkanFlex::sleep() {
    if (role_ == Role::Observer) return;
    explicit_pacing_.store(static_cast<std::uint8_t>(ExplicitPacing::Sleep), std::memory_order_release);
    core_paced_pending_.store(false, std::memory_order_release);
    if (role_ == Role::Delegate) {
        if (dxvk_low_latency_ && enabled_from_control())
            (void)dxvk_low_latency_->LatencySleep();
        return;
    }
    (void)pace_frame(INVALID_ID, false);
}

void VulkanFlex::sleep_with_frame_id(std::uint64_t frame_id) {
    if (role_ == Role::Observer) return;
    explicit_pacing_.store(static_cast<std::uint8_t>(ExplicitPacing::Sleep), std::memory_order_release);
    core_paced_pending_.store(false, std::memory_order_release);
    if (role_ == Role::Delegate) {
        if (dxvk_low_latency_ && enabled_from_control())
            (void)dxvk_low_latency_->LatencySleep();
        last_started_frame_id_.store(frame_id, std::memory_order_release);
        return;
    }
    (void)pace_frame(frame_id, false);
}

void VulkanFlex::bind_vkd3d_queue(ID3D12CommandQueue* queue, bool authoritative) noexcept {
    if (transport_ != Transport::Vkd3dD3D12 || !vkd3d_interop_ || !queue)
        return;

    const auto current = vkd3d_d3d_queue_.load(std::memory_order_acquire);
    if (current == queue)
        return;
    if (current != nullptr && !authoritative)
        return;

    // Queue discovery is a one-time/control-plane operation. If two marker
    // threads race during startup, one simply fails open rather than blocking.
    if (vkd3d_queue_bind_gate_.test_and_set(std::memory_order_acquire))
        return;

    VkQueue vk_queue = VK_NULL_HANDLE;
    UINT32 family = 0xffffffffu;
    const HRESULT hr = vkd3d_interop_->GetVulkanQueueInfo(queue, &vk_queue, &family);
    if (SUCCEEDED(hr) && vk_queue != VK_NULL_HANDLE) {
        vkd3d_vk_queue_.store(vk_queue, std::memory_order_relaxed);
        vkd3d_queue_family_.store(family, std::memory_order_relaxed);
        vkd3d_d3d_queue_.store(queue, std::memory_order_release);
        spdlog::info(
            "VulkanFlex VKD3D queue bound: D3D12Queue={}, VkQueue={}, family={}, authoritative={}",
            static_cast<const void*>(queue), static_cast<const void*>(vk_queue), family, authoritative);
    } else if (current == nullptr) {
        spdlog::debug("VulkanFlex VKD3D queue discovery deferred: hr=0x{:08x}",
                      static_cast<unsigned>(hr));
    }

    vkd3d_queue_bind_gate_.clear(std::memory_order_release);
}

void VulkanFlex::publish_bridge_present(std::uint64_t frame_id) noexcept {
    if ((transport_ != Transport::Vkd3dD3D12 && transport_ != Transport::DxvkD3D11) ||
        frame_id == 0 || frame_id == INVALID_ID)
        return;

    // Synchronous and OOB present markers can describe the same render frame.
    // Deduplicate before either publishing executor feedback or observer data.
    const auto previous = last_bridge_feedback_frame_.exchange(frame_id, std::memory_order_acq_rel);
    if (previous == frame_id)
        return;

    const auto now = get_timestamp();
    if (role_ == Role::Observer) {
        observer_last_frame_.store(frame_id, std::memory_order_relaxed);
        observer_last_present_ns_.store(now, std::memory_order_relaxed);
        observer_present_count_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    core_paced_pending_.store(false, std::memory_order_release);
    publish_feedback(frame_id, now, epoch_.load(std::memory_order_acquire));
}

void VulkanFlex::set_marker(IUnknown*, MarkerParams* marker_params) {
    if (!marker_params)
        return;

    if (role_ == Role::Observer) {
        observe_canonical_marker(marker_params);
        return;
    }

    if (role_ == Role::Delegate) {
        if (dxvk_low_latency_) {
            const HRESULT hr = dxvk_low_latency_->SetLatencyMarker(
                marker_params->frame_id, static_cast<UINT32>(marker_params->marker_type));
            if (FAILED(hr)) {
                spdlog::debug(
                    "VulkanFlex DXVK delegated marker failed: type={} frame={} hr=0x{:08x}",
                    static_cast<unsigned>(marker_params->marker_type),
                    marker_params->frame_id, static_cast<unsigned>(hr));
            }
        }
        return;
    }

    if (transport_ == Transport::Vkd3dD3D12 && marker_params->marker_type == MarkerType::PRESENT_END) {
        publish_bridge_present(marker_params->frame_id);
        return;
    }

    if (marker_params->marker_type != MarkerType::SIMULATION_START)
        return;

    const auto mode = static_cast<ExplicitPacing>(explicit_pacing_.load(std::memory_order_acquire));
    if (mode == ExplicitPacing::Sleep)
        return; // explicit Sleep is the sole pacing owner

    explicit_pacing_.store(static_cast<std::uint8_t>(ExplicitPacing::Marker), std::memory_order_release);

    // If core acquire already paced the currently open frame, switch ownership
    // for subsequent frames without issuing a second sleep in this frame.
    if (core_paced_pending_.load(std::memory_order_acquire) &&
        core_paced_pending_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    (void)pace_frame(marker_params->frame_id, false);
}

void VulkanFlex::set_async_marker(MarkerParams* marker_params) {
    if (!marker_params)
        return;
    if (role_ == Role::Observer) {
        observe_canonical_marker(marker_params);
        return;
    }
    if (role_ == Role::Delegate) {
        if (dxvk_low_latency_)
            (void)dxvk_low_latency_->SetLatencyMarker(
                marker_params->frame_id, static_cast<UINT32>(marker_params->marker_type));
        return;
    }
    if (transport_ == Transport::Vkd3dD3D12 &&
        marker_params->marker_type == MarkerType::OUT_OF_BAND_PRESENT_END) {
        publish_bridge_present(marker_params->frame_id);
    }
}

void VulkanFlex::set_async_marker_on_queue(
    ID3D12CommandQueue* queue, MarkerParams* marker_params) {
    if (!marker_params)
        return;

    const bool present_queue =
        marker_params->marker_type == MarkerType::OUT_OF_BAND_PRESENT_START ||
        marker_params->marker_type == MarkerType::OUT_OF_BAND_PRESENT_END;
    bind_vkd3d_queue(queue, present_queue);
    set_async_marker(marker_params);
}

void VulkanFlex::observe_d3d12_queue(ID3D12CommandQueue* queue) {
    bind_vkd3d_queue(queue, false);
}

void VulkanFlex::observe_canonical_frame_start(uint64_t frame_id) {
    if (role_ != Role::Observer || frame_id == 0 || frame_id == INVALID_ID) return;
    observer_last_frame_.store(frame_id, std::memory_order_relaxed);
    observer_frame_count_.fetch_add(1, std::memory_order_relaxed);
}

void VulkanFlex::observe_canonical_marker(const MarkerParams* marker) {
    if (role_ != Role::Observer || !marker || marker->frame_id == 0 ||
        marker->frame_id == INVALID_ID) return;

    observer_last_frame_.store(marker->frame_id, std::memory_order_relaxed);
    if (marker->marker_type == MarkerType::PRESENT_END ||
        marker->marker_type == MarkerType::OUT_OF_BAND_PRESENT_END) {
        publish_bridge_present(marker->frame_id);
    }
}

void VulkanFlex::observe_canonical_presentation(const PresentationParams* presentation) {
    if (!presentation || presentation->render_frame_id == 0 ||
        presentation->present_frame_id == 0) return;

    // Presentation telemetry is valid for translation Observer/Delegate roles.
    // It never enters the scheduler, delegated sleep, or completion-feedback
    // path, so an interpolated present cannot become a second pacing event.
    if (role_ != Role::Observer && role_ != Role::Delegate) return;

    observer_last_frame_.store(presentation->render_frame_id, std::memory_order_relaxed);
    observer_last_presentation_.store(presentation->present_frame_id, std::memory_order_relaxed);
    observer_presentation_count_.fetch_add(1, std::memory_order_relaxed);
    if (presentation->interpolated)
        observer_generated_present_count_.fetch_add(1, std::memory_order_relaxed);
}

void VulkanFlex::observe_structural_stall(
    StructuralStallKind kind, std::uint64_t duration_ns,
    std::uint64_t end_timestamp_ns) noexcept {
    if (!structural_stall_enabled_ || duration_ns < pipeline_threshold_ns_ ||
        end_timestamp_ns == 0) {
        return;
    }

    const auto active_epoch = epoch_.load(std::memory_order_acquire);
    const auto start = structural_stall_cursor_.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t probe = 0; probe < kStructuralStallSlots; ++probe) {
        auto& slot = structural_stall_slots_[(start + probe) % kStructuralStallSlots];
        std::uint64_t expected = 0;
        if (!slot.end_timestamp_ns.compare_exchange_strong(
                expected, kStallSlotPublishing, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }

        slot.duration_ns.store(duration_ns, std::memory_order_relaxed);
        slot.epoch.store(active_epoch, std::memory_order_relaxed);
        slot.kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
        slot.end_timestamp_ns.store(end_timestamp_ns, std::memory_order_release);
        structural_pipeline_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Pipeline creation is a rare/control-plane signal. If all fixed slots are
    // occupied, drop evidence rather than blocking a driver or compilation thread.
    structural_dropped_events_.fetch_add(1, std::memory_order_relaxed);
}

void VulkanFlex::queue_precision_feedback(
    SwapchainLane* lane,
    std::uint64_t present_id,
    std::uint64_t frame_id,
    std::uint64_t frame_epoch,
    std::uint64_t fallback_timestamp_ns,
    bool uses_present_id2,
    bool timing_requested) noexcept {
    if (!lane || present_id == 0 || frame_id == 0 || frame_epoch == 0)
        return;

    // UINT64_MAX is reserved internally as the transient publication marker.
    // Vulkan present IDs are 64-bit, so fail open rather than silently losing
    // the (theoretical) terminal ID value if an application ever uses it.
    if (present_id == kPrecisionSlotPublishing) {
        publish_feedback(frame_id, fallback_timestamp_ns, frame_epoch);
        precision_fallbacks_.fetch_add(1, std::memory_order_relaxed);
        if (frame_epoch == epoch_.load(std::memory_order_acquire))
            lane->fallback_completions.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    static_assert((kPrecisionPending & (kPrecisionPending - 1)) == 0,
                  "precision pending capacity must be a power of two");

    const auto start = static_cast<std::size_t>((frame_id ^ present_id) & (kPrecisionPending - 1));
    PrecisionPending* target = nullptr;
    std::uint64_t replaced_present_id = 0;
    bool claimed_free = false;

    // Prefer an actually free slot. The old round-robin cursor could overwrite
    // an unresolved present while another slot was already free, needlessly
    // degrading completion quality and paying a locked fetch_add every frame.
    for (std::size_t probe = 0; probe < kPrecisionPending; ++probe) {
        auto& candidate = lane->precision_pending[(start + probe) & (kPrecisionPending - 1)];
        if (candidate.present_id.load(std::memory_order_acquire) != 0)
            continue;
        std::uint64_t expected = 0;
        if (candidate.present_id.compare_exchange_strong(
                expected, kPrecisionSlotPublishing,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            target = &candidate;
            claimed_free = true;
            break;
        }
    }

    // Only a genuinely full ring may evict. Claim an occupied publication word
    // with CAS so a concurrent completion poller can win cleanly; never stomp a
    // slot another producer is currently publishing.
    if (!target) {
        for (std::size_t probe = 0; probe < kPrecisionPending; ++probe) {
            auto& candidate = lane->precision_pending[(start + probe) & (kPrecisionPending - 1)];
            auto observed = candidate.present_id.load(std::memory_order_acquire);
            if (observed == 0 || observed == kPrecisionSlotPublishing)
                continue;
            if (candidate.present_id.compare_exchange_strong(
                    observed, kPrecisionSlotPublishing,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                target = &candidate;
                replaced_present_id = observed;
                break;
            }
        }
    }

    if (!target) {
        // All slots are transiently owned by concurrent producers/pollers. Never
        // block vkQueuePresentKHR; preserve one CPU-return completion sample.
        publish_feedback(frame_id, fallback_timestamp_ns, frame_epoch);
        precision_fallbacks_.fetch_add(1, std::memory_order_relaxed);
        if (frame_epoch == epoch_.load(std::memory_order_acquire))
            lane->fallback_completions.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (claimed_free) {
        // Publish occupancy before the final present_id. A racing acquire may
        // perform one harmless scan, but it cannot skip a fully published slot.
        lane->precision_pending_count.fetch_add(1, std::memory_order_relaxed);
    } else if (replaced_present_id != 0) {
        const auto old_epoch = target->epoch.load(std::memory_order_relaxed);
        publish_feedback(
            target->frame_id.load(std::memory_order_relaxed),
            target->fallback_timestamp_ns.load(std::memory_order_relaxed),
            old_epoch);
        precision_fallbacks_.fetch_add(1, std::memory_order_relaxed);
        if (old_epoch == epoch_.load(std::memory_order_acquire))
            lane->fallback_completions.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint8_t flags = 0;
    if (uses_present_id2) flags |= kPrecisionUsesId2;
    if (timing_requested) flags |= kPrecisionTimingRequested;
    target->frame_id.store(frame_id, std::memory_order_relaxed);
    target->epoch.store(frame_epoch, std::memory_order_relaxed);
    target->fallback_timestamp_ns.store(fallback_timestamp_ns, std::memory_order_relaxed);
    target->flags.store(flags, std::memory_order_relaxed);
    target->polls.store(0, std::memory_order_relaxed);
    target->present_id.store(present_id, std::memory_order_release);
}

void VulkanFlex::resolve_precision_feedback(VkDevice device, SwapchainLane* lane) noexcept {
    if (!present_precision_enabled_ || !lane || device == VK_NULL_HANDLE ||
        lane->precision_pending_count.load(std::memory_order_relaxed) == 0) {
        return;
    }

    const auto swapchain = lane->swapchain.load(std::memory_order_acquire);
    if (swapchain == VK_NULL_HANDLE || !ensure_precision_profile(device, lane))
        return;

    // The dispatch table and swapchain flags are immutable after the profile's
    // release-publication. Avoid global registry generation loads/scans on every
    // acquire-side precision probe.
    const auto& dispatch = lane->precision_dispatch;
    const auto swapchain_flags = lane->precision_swapchain_flags;
    const auto active_epoch = epoch_.load(std::memory_order_acquire);
    PresentPrecisionCapabilities caps{};
    caps.present_id = dispatch.present_id_enabled;
    caps.present_id2 = dispatch.present_id2_enabled;
    caps.present_wait = dispatch.present_wait_enabled && dispatch.wait_for_present;
    caps.present_wait2 = dispatch.present_wait2_enabled && dispatch.wait_for_present2;
    caps.present_timing = dispatch.present_timing_enabled && dispatch.get_past_presentation_timing;

    // One acquire-side probe can discover several presentations that completed
    // between two CPU observations. Do not feed a burst of near-identical CPU
    // timestamps into LatencyFleX: account every retirement, but publish one
    // representative estimator sample, preferring precise evidence.
    completionfeedback::Candidate feedback_batch{};

    auto retire_slot = [&](std::uint64_t completed_frame,
                           std::uint64_t completed_epoch, std::uint64_t timestamp_ns,
                           bool precise) noexcept {
        // precision_pending_count is advisory; present_id is authoritative. Use
        // a saturating CAS decrement so a concurrent destroy/reset can never
        // underflow the hint and permanently force the expensive scan path.
        auto pending = lane->precision_pending_count.load(std::memory_order_relaxed);
        while (pending != 0 &&
               !lane->precision_pending_count.compare_exchange_weak(
                   pending, pending - 1,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {}

        completionfeedback::observe(
            feedback_batch, completed_frame, timestamp_ns,
            completed_epoch, active_epoch, precise);

        if (completed_epoch == active_epoch) {
            if (precise)
                lane->precise_completions.fetch_add(1, std::memory_order_relaxed);
            else
                lane->fallback_completions.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // First consume any asynchronous presentation-engine timing reports. The
    // returned timestamps may use a presentation-specific clock domain, so the
    // report is used only as authoritative completion evidence. One QPC-domain
    // timestamp is taken after the whole probe batch has been retired.
#ifdef VK_EXT_present_timing
    bool timing_probe_needed = false;
    if (caps.present_timing &&
        (swapchain_flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) != 0) {
        for (const auto& slot : lane->precision_pending) {
            const auto id = slot.present_id.load(std::memory_order_acquire);
            if (id == 0 || id == kPrecisionSlotPublishing)
                continue;
            const auto flags = slot.flags.load(std::memory_order_relaxed);
            if ((flags & (kPrecisionUsesId2 | kPrecisionTimingRequested)) ==
                (kPrecisionUsesId2 | kPrecisionTimingRequested)) {
                timing_probe_needed = true;
                break;
            }
        }
    }

    if (timing_probe_needed) {
        std::array<std::array<VkPresentStageTimeEXT, 4>, kPrecisionPending> stages{};
        std::array<VkPastPresentationTimingEXT, kPrecisionPending> timings{};
        for (std::size_t i = 0; i < timings.size(); ++i) {
            timings[i].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
            timings[i].presentStageCount = static_cast<std::uint32_t>(stages[i].size());
            timings[i].pPresentStages = stages[i].data();
        }

        VkPastPresentationTimingInfoEXT query{};
        query.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
        query.swapchain = swapchain;
        VkPastPresentationTimingPropertiesEXT results{};
        results.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
        results.presentationTimingCount = static_cast<std::uint32_t>(timings.size());
        results.pPresentationTimings = timings.data();

        const VkResult result = dispatch.get_past_presentation_timing(device, &query, &results);
        if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
            const auto count = std::min<std::uint32_t>(
                results.presentationTimingCount, static_cast<std::uint32_t>(timings.size()));
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!timings[i].reportComplete || timings[i].presentId == 0)
                    continue;
                for (auto& slot : lane->precision_pending) {
                    auto expected = timings[i].presentId;
                    const auto flags = slot.flags.load(std::memory_order_relaxed);
                    if ((flags & (kPrecisionUsesId2 | kPrecisionTimingRequested)) !=
                        (kPrecisionUsesId2 | kPrecisionTimingRequested)) {
                        continue;
                    }
                    if (slot.present_id.compare_exchange_strong(
                            expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
                        const auto completed_frame = slot.frame_id.load(std::memory_order_relaxed);
                        const auto completed_epoch = slot.epoch.load(std::memory_order_relaxed);
                        retire_slot(completed_frame, completed_epoch, 0, true);
                        precision_timing_hits_.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        }
    }
#endif

    const auto probe_start = static_cast<std::size_t>(
        lane->precision_probe_cursor.load(std::memory_order_relaxed) & (kPrecisionPending - 1));
    std::size_t visited = 0;
    std::uint8_t driver_probes = 0;
    for (; visited < kPrecisionPending && driver_probes < kPrecisionWaitProbeBudget; ++visited) {
        auto& slot = lane->precision_pending[(probe_start + visited) & (kPrecisionPending - 1)];
        const auto present_id = slot.present_id.load(std::memory_order_acquire);
        if (present_id == 0 || present_id == kPrecisionSlotPublishing)
            continue;

        const auto flags = slot.flags.load(std::memory_order_relaxed);
        const bool uses_id2 = (flags & kPrecisionUsesId2) != 0;
        const bool timing_requested = (flags & kPrecisionTimingRequested) != 0;
        PresentPrecisionObservation observation{};
        observation.has_present_id = true;
        observation.uses_present_id2 = uses_id2;
        observation.timing_requested = timing_requested;
        observation.swapchain_flags = swapchain_flags;
        const auto tier = select_present_precision_tier(caps, observation);

        // Every pending precision slot is VF2+, so a non-empty slot consumes one
        // bounded driver-probe unit. This caps acquire-side driver re-entry under
        // pressure while the rotating cursor guarantees fairness across slots.
        ++driver_probes;
        bool completed = false;
        if (tier >= PresentPrecisionTier::PresentWait2 && caps.present_wait2 && uses_id2 &&
            (swapchain_flags & VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR) != 0) {
#ifdef VK_KHR_present_wait2
            VkPresentWait2InfoKHR wait_info{};
            wait_info.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR;
            wait_info.presentId = present_id;
            wait_info.timeout = 0; // sensor only; queue-depth policy owns any pacing delay
            completed = dispatch.wait_for_present2(device, swapchain, &wait_info) == VK_SUCCESS;
#endif
        } else if (tier >= PresentPrecisionTier::PresentWait && caps.present_wait) {
            completed = dispatch.wait_for_present(device, swapchain, present_id, 0) == VK_SUCCESS;
        }

        if (completed) {
            auto expected = present_id;
            if (slot.present_id.compare_exchange_strong(
                    expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
                const auto completed_frame = slot.frame_id.load(std::memory_order_relaxed);
                const auto completed_epoch = slot.epoch.load(std::memory_order_relaxed);
                retire_slot(completed_frame, completed_epoch, 0, true);
                if (tier >= PresentPrecisionTier::PresentWait2)
                    precision_wait2_hits_.fetch_add(1, std::memory_order_relaxed);
                else
                    precision_wait_hits_.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }

        const auto polls = static_cast<std::uint8_t>(
            slot.polls.fetch_add(1, std::memory_order_relaxed) + 1);
        if (polls < kPrecisionMaxPolls)
            continue;

        auto expected = present_id;
        if (slot.present_id.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
            const auto completed_frame = slot.frame_id.load(std::memory_order_relaxed);
            const auto completed_epoch = slot.epoch.load(std::memory_order_relaxed);
            const auto fallback_ns = slot.fallback_timestamp_ns.load(std::memory_order_relaxed);
            retire_slot(completed_frame, completed_epoch, fallback_ns, false);
            precision_fallbacks_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    lane->precision_probe_cursor.store(
        static_cast<std::uint8_t>((probe_start + visited) & (kPrecisionPending - 1)),
        std::memory_order_relaxed);

    if (feedback_batch.valid) {
        const auto timestamp_ns = feedback_batch.precise
            ? get_timestamp() : feedback_batch.timestamp_ns;
        publish_feedback(
            feedback_batch.frame_id, timestamp_ns, feedback_batch.epoch,
            feedback_batch.completion_count);
    }
}

void VulkanFlex::update_queue_pressure(SwapchainLane* lane) noexcept {
    const auto clear_delay = [&]() noexcept {
        // Zero is the dominant state. Avoid dirtying the scheduler cache line on
        // every acquire when there is no pressure to publish.
        if (queue_pressure_delay_ns_.load(std::memory_order_relaxed) != 0)
            queue_pressure_delay_ns_.store(0, std::memory_order_release);
    };

    if (!queue_pressure_enabled_ || !lane) {
        clear_delay();
        return;
    }

    const auto active_epoch = epoch_.load(std::memory_order_acquire);
    auto previous_pressure_epoch = lane->pressure_epoch.load(std::memory_order_acquire);
    if (previous_pressure_epoch != active_epoch) {
        // Epoch rollover is rare. The steady path is a read; CAS elects exactly
        // one thread to reset per-epoch pressure statistics when rollover occurs.
        while (previous_pressure_epoch != active_epoch) {
            if (lane->pressure_epoch.compare_exchange_weak(
                    previous_pressure_epoch, active_epoch,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                lane->precise_completions.store(0, std::memory_order_relaxed);
                lane->fallback_completions.store(0, std::memory_order_relaxed);
                lane->max_pending_seen.store(0, std::memory_order_relaxed);
                queue_pressure_trust_logged_.store(false, std::memory_order_relaxed);
                break;
            }
        }
    }

    // The occupancy hint is published before present_id and retired only after
    // a slot is claimed. It is deliberately advisory: a racing reader may lag
    // one observation, which only fails open for one governor update. The
    // overwhelmingly common target=1, pending<=1 case can therefore skip eight
    // acquire loads and the entire decision path; only possible pressure pays
    // the authoritative slot scan.
    const auto pending_hint = lane->precision_pending_count.load(std::memory_order_relaxed);
    const auto target_hint = static_cast<std::uint32_t>(queue_target_presents_);

    auto lane_hint_max = lane->max_pending_seen.load(std::memory_order_relaxed);
    while (pending_hint > lane_hint_max &&
           !lane->max_pending_seen.compare_exchange_weak(
               lane_hint_max, pending_hint, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    auto global_hint_max = queue_pressure_max_pending_.load(std::memory_order_relaxed);
    while (pending_hint > global_hint_max &&
           !queue_pressure_max_pending_.compare_exchange_weak(
               global_hint_max, pending_hint, std::memory_order_relaxed, std::memory_order_relaxed)) {}

    if (pending_hint <= target_hint) {
        clear_delay();
        return;
    }

    std::uint32_t pending = 0;
    for (const auto& slot : lane->precision_pending) {
        const auto id = slot.present_id.load(std::memory_order_acquire);
        if (id != 0 && id != kPrecisionSlotPublishing &&
            slot.epoch.load(std::memory_order_relaxed) == active_epoch)
            ++pending;
    }

    auto lane_max = lane->max_pending_seen.load(std::memory_order_relaxed);
    while (pending > lane_max &&
           !lane->max_pending_seen.compare_exchange_weak(
               lane_max, pending, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    auto global_max = queue_pressure_max_pending_.load(std::memory_order_relaxed);
    while (pending > global_max &&
           !queue_pressure_max_pending_.compare_exchange_weak(
               global_max, pending, std::memory_order_relaxed, std::memory_order_relaxed)) {}

    const auto started = queue_started_frames_.load(std::memory_order_relaxed);
    const auto completed = queue_completed_frames_.load(std::memory_order_relaxed);
    QueuePressureSample sample{};
    sample.pending_presents = pending;
    sample.cpu_ahead = started > completed ? started - completed : 0;
    sample.precise_completions = lane->precise_completions.load(std::memory_order_relaxed);
    sample.fallback_completions = lane->fallback_completions.load(std::memory_order_relaxed);
    sample.best_tier = static_cast<PresentPrecisionTier>(
        lane->best_precision_tier.load(std::memory_order_relaxed));
    sample.observed_frame_time_ns = queue_observed_frame_time_ns_.load(std::memory_order_relaxed);

    QueuePressureConfig config{};
    config.mode = queue_pressure_mode_;
    config.target_pending_presents = queue_target_presents_;
    config.max_delay_us = queue_max_delay_us_;
    const auto decision = select_queue_pressure_delay(config, sample);
    if (decision.delay_ns != 0)
        queue_pressure_delay_ns_.store(decision.delay_ns, std::memory_order_release);
    else
        clear_delay();

    if (decision.trusted &&
        !queue_pressure_trust_logged_.exchange(true, std::memory_order_acq_rel)) {
        spdlog::info(
            "VulkanFlex queue governor armed: tier={}, precise={}, fallback={}, target={}, max_delay_us={}",
            present_precision_tier_name(sample.best_tier), sample.precise_completions,
            sample.fallback_completions, queue_target_presents_, queue_max_delay_us_);
    }
    if (decision.pressured) {
        spdlog::trace(
            "VulkanFlex queue pressure: pending={}, cpu_ahead={}, units={}, delay_us={}",
            sample.pending_presents, sample.cpu_ahead, decision.pressure_units,
            decision.delay_ns / 1000ull);
    }
}

void VulkanFlex::on_acquire(VkDevice device, VkSwapchainKHR swapchain, std::uint32_t image_index) noexcept {
    if (!enabled_from_control() || image_index >= kImagesPerLane)
        return;

    auto* lane = lane_for(swapchain, true);
    if (!lane) return;

    // One swapchain owns completion feedback for both core and explicit modes.
    // Claiming it outside the core-only branch also prevents an instrumented
    // multi-window title from feeding duplicate EndFrame samples.
    auto primary = primary_swapchain_.load(std::memory_order_acquire);
    if (primary == VK_NULL_HANDLE) {
        VkSwapchainKHR expected = VK_NULL_HANDLE;
        if (primary_swapchain_.compare_exchange_strong(
                expected, swapchain, std::memory_order_acq_rel, std::memory_order_acquire)) {
            primary = swapchain;
        } else {
            primary = expected;
        }
    }

    if (primary == swapchain) {
        resolve_precision_feedback(device, lane);
        update_queue_pressure(lane);
    }

    std::uint64_t frame_id = 0;
    const auto explicit_mode = static_cast<ExplicitPacing>(explicit_pacing_.load(std::memory_order_acquire));
    if (explicit_mode == ExplicitPacing::None && core_pacing_enabled_) {
        // Only the primary swapchain is allowed to own core-WSI pacing.
        // Auxiliary swapchains observe the last started frame but can never
        // create an extra sleep.
        frame_id = primary == swapchain
            ? pace_frame(INVALID_ID, true)
            : last_started_frame_id_.load(std::memory_order_acquire);
    } else {
        frame_id = last_started_frame_id_.load(std::memory_order_acquire);
    }

    const auto active_epoch = epoch_.load(std::memory_order_acquire);
    lane->frame_epochs[image_index].store(active_epoch, std::memory_order_relaxed);
    lane->frame_ids[image_index].store(frame_id, std::memory_order_release);
}

VulkanFlex::PresentTicket VulkanFlex::begin_present(
    VkSwapchainKHR swapchain, std::uint32_t image_index) noexcept {
    PresentTicket ticket{};
    if (image_index >= kImagesPerLane)
        return ticket;

    auto* lane = lane_for(swapchain, false);
    if (!lane)
        return ticket;

    // Claim the image mapping before entering the real vkQueuePresentKHR. Once
    // the driver returns, another thread may immediately reacquire the same
    // image; consuming the mapping only in the post-hook can therefore steal
    // the next frame's identity. frame_id is the publication/ownership word;
    // its acquire half makes the separately stored epoch visible.
    const auto frame_id = lane->frame_ids[image_index].exchange(0, std::memory_order_acq_rel);
    if (frame_id == 0)
        return ticket;
    const auto frame_epoch = lane->frame_epochs[image_index].load(std::memory_order_relaxed);
    lane->frame_epochs[image_index].store(0, std::memory_order_relaxed);
    if (frame_epoch == 0)
        return ticket;

    const auto primary = primary_swapchain_.load(std::memory_order_acquire);
    if (primary != VK_NULL_HANDLE && primary != swapchain)
        return ticket;

    ticket.swapchain = swapchain;
    ticket.frame_id = frame_id;
    ticket.epoch = frame_epoch;
    ticket.image_index = image_index;
    ticket.lane_index = static_cast<std::uint8_t>(lane - swapchains_.data());
    return ticket;
}

void VulkanFlex::complete_present(
    VkDevice device,
    const PresentTicket& ticket,
    std::uint64_t present_id,
    bool uses_present_id2,
    bool timing_requested) noexcept {
    if (!ticket || ticket.lane_index >= kSwapchainLanes)
        return;

    // If backend control rolled to a new epoch while the driver was inside
    // vkQueuePresentKHR, this completion belongs to the old execution epoch and
    // must not clear/pollute current pacing state.
    if (ticket.epoch != epoch_.load(std::memory_order_acquire))
        return;

    auto& lane = swapchains_[ticket.lane_index];
    if (lane.swapchain.load(std::memory_order_acquire) != ticket.swapchain)
        return;

    const auto primary = primary_swapchain_.load(std::memory_order_acquire);
    if (primary != VK_NULL_HANDLE && primary != ticket.swapchain)
        return;

    core_paced_pending_.store(false, std::memory_order_release);
    const auto fallback_timestamp = get_timestamp();

    PresentPrecisionTier tier = PresentPrecisionTier::CoreWsi;
    if (present_precision_enabled_ && present_id != 0 &&
        ensure_precision_profile(device, &lane)) {
        const auto& dispatch = lane.precision_dispatch;
        PresentPrecisionCapabilities caps{};
        caps.present_id = dispatch.present_id_enabled;
        caps.present_id2 = dispatch.present_id2_enabled;
        caps.present_wait = dispatch.present_wait_enabled && dispatch.wait_for_present;
        caps.present_wait2 = dispatch.present_wait2_enabled && dispatch.wait_for_present2;
        caps.present_timing = dispatch.present_timing_enabled && dispatch.get_past_presentation_timing;

        PresentPrecisionObservation observation{};
        observation.has_present_id = true;
        observation.uses_present_id2 = uses_present_id2;
        observation.timing_requested = timing_requested;
        observation.swapchain_flags = lane.precision_swapchain_flags;
        tier = select_present_precision_tier(caps, observation);
    }

    const auto tier_value = static_cast<std::uint8_t>(tier);
    auto previous_tier = lane.best_precision_tier.load(std::memory_order_relaxed);
    while (tier_value > previous_tier &&
           !lane.best_precision_tier.compare_exchange_weak(
               previous_tier, tier_value,
               std::memory_order_relaxed, std::memory_order_relaxed)) {}
    if (tier_value > previous_tier) {
        spdlog::info("VulkanFlex present precision upgraded: {}", present_precision_tier_name(tier));
    }

    if (tier >= PresentPrecisionTier::PresentWait) {
        queue_precision_feedback(
            &lane, present_id, ticket.frame_id, ticket.epoch, fallback_timestamp,
            uses_present_id2, timing_requested);
        return;
    }

    // VF0/VF1 retain the non-blocking CPU-return completion sample.
    publish_feedback(ticket.frame_id, fallback_timestamp, ticket.epoch);
}

void VulkanFlex::on_destroy_swapchain(VkSwapchainKHR swapchain) noexcept {
    auto* lane = lane_for(swapchain, false);
    if (!lane) return;

    // Unpublish identity first. New/TLS lookups now fail immediately while the
    // rare teardown path clears payload state behind the retired lane.
    lane->swapchain.store(VK_NULL_HANDLE, std::memory_order_release);
    for (std::size_t i = 0; i < kImagesPerLane; ++i) {
        lane->frame_ids[i].store(0, std::memory_order_relaxed);
        lane->frame_epochs[i].store(0, std::memory_order_relaxed);
    }
    for (auto& pending : lane->precision_pending)
        pending.present_id.store(0, std::memory_order_release);
    lane->precision_pending_count.store(0, std::memory_order_relaxed);
    lane->precision_probe_cursor.store(0, std::memory_order_relaxed);
    lane->precision_profile_state.store(kPrecisionProfileUninitialized, std::memory_order_relaxed);
    lane->precision_device = VK_NULL_HANDLE;
    lane->precision_swapchain_flags = 0;
    lane->precision_dispatch = {};
    lane->best_precision_tier.store(0, std::memory_order_relaxed);
    lane->precise_completions.store(0, std::memory_order_relaxed);
    lane->fallback_completions.store(0, std::memory_order_relaxed);
    lane->max_pending_seen.store(0, std::memory_order_relaxed);
    lane->pressure_epoch.store(0, std::memory_order_relaxed);

    auto expected = swapchain;
    if (primary_swapchain_.compare_exchange_strong(
            expected, VK_NULL_HANDLE, std::memory_order_acq_rel, std::memory_order_acquire)) {
        core_paced_pending_.store(false, std::memory_order_release);
        queue_pressure_delay_ns_.store(0, std::memory_order_release);
    }
}
