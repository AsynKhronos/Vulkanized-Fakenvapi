#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace policy {

enum class LoggingLevel : std::uint8_t {
    Off,
    Info,
    Trace,
};

enum class GraphicsApi : std::uint8_t {
    D3D11,
    D3D12,
    Vulkan,
};

enum class InputFrontend : std::uint8_t {
    Auto,
    Reflex,
    VkNvLowLatency2,
    XeLL,
    AntiLag2,
};

enum class Backend : std::uint8_t {
    Auto,
    AntiLag2,
    XeLL,
    AmdAntiLagVk,
    LatencyFlex,
    NativeReflex,
    VulkanFlex,
};

enum class AutoBool : std::uint8_t {
    Auto,
    Disabled,
    Enabled,
};

enum class VulkanSpoofing : std::uint8_t {
    Off,
    Selective,
    Aggressive,
};

enum class NormalizedMarker : std::uint8_t {
    SimulationStart,
    SimulationEnd,
    RenderSubmitStart,
    RenderSubmitEnd,
    PresentStart,
    PresentEnd,
    InputSample,
    TriggerFlash,
    PcLatencyPing,
    OutOfBandRenderSubmitStart,
    OutOfBandRenderSubmitEnd,
    OutOfBandPresentStart,
    OutOfBandPresentEnd,
};

template <typename T, std::size_t Capacity>
struct FixedOrder {
    std::array<T, Capacity> values{};
    std::uint8_t size = 0;

    constexpr void push_back(T value) noexcept {
        if (size < Capacity) {
            values[size++] = value;
        }
    }

    [[nodiscard]] constexpr bool contains(T value) const noexcept {
        for (std::uint8_t i = 0; i < size; ++i) {
            if (values[i] == value) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr T operator[](std::size_t index) const noexcept {
        return values[index];
    }
};

using InputPriority = FixedOrder<InputFrontend, 4>;
using BackendOrder = FixedOrder<Backend, 6>;

struct GeneralPolicy {
    LoggingLevel logging = LoggingLevel::Info;
    bool allow_fallback = true;
    bool runtime_switching = true;
};

struct InputPolicy {
    InputFrontend mode = InputFrontend::Auto;
    InputPriority priority = [] {
        InputPriority value;
        value.push_back(InputFrontend::Reflex);
        value.push_back(InputFrontend::XeLL);
        value.push_back(InputFrontend::AntiLag2);
        value.push_back(InputFrontend::VkNvLowLatency2);
        return value;
    }();
    std::uint8_t minimum_quality = 60;
};

struct OutputPolicy {
    Backend d3d11 = Backend::Auto;
    Backend d3d12 = Backend::Auto;
    Backend vulkan = Backend::Auto;
};

struct BackendOrderPolicy {
    BackendOrder d3d11 = [] {
        BackendOrder value;
        value.push_back(Backend::AntiLag2);
        value.push_back(Backend::LatencyFlex);
        return value;
    }();
    BackendOrder d3d12 = [] {
        BackendOrder value;
        // The direct vkd3d/Vulkan backend is injected by policy only when
        // prefer_native_extensions is enabled. Keep it out of the ordinary
        // fallback order so disabling native extensions really disables the
        // low-level D3D12 path.
        value.push_back(Backend::AntiLag2);
        value.push_back(Backend::XeLL);
        value.push_back(Backend::LatencyFlex);
        return value;
    }();
    BackendOrder vulkan = [] {
        BackendOrder value;
        value.push_back(Backend::VulkanFlex);
        value.push_back(Backend::AmdAntiLagVk);
        value.push_back(Backend::LatencyFlex);
        return value;
    }();
};

struct VulkanPolicy {
    VulkanSpoofing spoofing = VulkanSpoofing::Selective;
    AutoBool expose_nv_low_latency2 = AutoBool::Auto;
    AutoBool expose_amd_anti_lag = AutoBool::Auto;
    bool prefer_native_extensions = true;
};

struct VulkanFlexPolicy {
    bool enabled = true;
    bool core_pacing = true;
    bool only_native_vulkan = true;
    // Safe cooperative D3D12->vkd3d-proton bridge. This never installs Vulkan
    // detours or submits Vulkan work. In Auto, stronger D3D12 executors (XeLL /
    // Anti-Lag 2) keep pacing ownership while VulkanFlex may attach as an
    // observer; VulkanFlex execution remains an explicit/fallback path.
    bool vkd3d_bridge = true;
    // DXVK cooperative bridge. Auto delegates pacing to DXVK's own
    // ID3DLowLatencyDevice when available and otherwise stays observer-only.
    // Enabled additionally permits the embedded VulkanFlex scheduler when the
    // DXVK low-latency interface is unavailable. Disabled is observer-only.
    bool dxvk_bridge = true;
    AutoBool dxvk_execution = AutoBool::Auto;
    // Present precision is non-invasive: Auto/Enabled consume only capabilities
    // and present metadata already enabled by the application/translation stack.
    AutoBool present_precision = AutoBool::Auto;
    // R3.7 soft queue-pressure governor. Auto arms only after reliable VF2+
    // completion evidence; Enabled bypasses warmup but still requires VF2+.
    // It never calls blocking present waits and is native-Vulkan executor only.
    AutoBool queue_pressure = AutoBool::Auto;
    std::uint8_t queue_target_presents = 1;
    std::uint32_t queue_max_delay_us = 2000;
    // R4.0 structural-stall shielding. Auto requires a warmed clean-frame
    // baseline plus a corroborating long pipeline-creation call. Enabled
    // relaxes only the warmup count; it never classifies from frametime alone.
    AutoBool structural_stall = AutoBool::Auto;
    std::uint32_t pipeline_threshold_us = 2000;
    std::uint32_t stall_max_compensation_us = 50000;
    std::uint32_t minimum_interval_us = 0;
    std::uint32_t max_sleep_us = 50000;
};

struct OpenXRPolicy {
    // R4.1 XRFlex is observer-only. xrWaitFrame remains the OpenXR runtime's
    // synchronization/throttling owner; the project never adds a second wait.
    bool enabled = true;
    bool timeline = true;
    // R4.2 maps XrTime to the project's canonical QPC-nanosecond host clock
    // only when the runtime exposes the enabled KHR conversion extension.
    bool clock_sync = true;
};

struct AudioPolicy {
    // R4.3 AudioFlex is observation-only. It records the WASAPI stream
    // configuration, engine-period queries, padding and IAudioClock telemetry
    // but never changes a period, buffer, format or stream state.
    bool enabled = true;
    bool wasapi_observer = true;
    bool clock = true;
    // R4.4 performs getter-only capability/clock probes after successful
    // initialization. It never changes stream format, period or buffering.
    bool active_probe = true;
    bool queue_model = true;
    std::uint32_t clock_probe_interval_ms = 250;
    // R4.5 init-time period negotiation. Auto is recommendation-only; Enabled
    // may lower an application's own IAudioClient3 shared-stream period to a
    // supported aligned target. Initialization is attempted exactly once.
    AutoBool adaptive_period = AutoBool::Auto;
    std::uint32_t period_target_us = 5000;
    std::uint8_t period_max_reduction_percent = 50;
};

struct HybridPolicy {
    // Hybrid is an intelligence layer for a single execution backend, never a
    // second pacing backend. The execution target may be direct Vulkan or XeLL. Every semantic aspect is selected independently during a short
    // startup evidence window and then frozen for the lifetime of the backend
    // session. Game-originated XeLL is a first-class input when present.
    bool xell_fusion = true;
    bool vulkan_fusion = true;
    bool startup_locked = true;
    std::uint16_t startup_observations = 64;
    std::uint8_t aspect_minimum_quality = 60;

    // 0.7.0 compatibility keys. They remain parseable so an older INI is not
    // rejected, but runtime aspect switching is deliberately disabled.
    bool aspect_adaptive = true;
    std::uint8_t aspect_switch_margin = 6;
    bool fill_missing_signals = true;
    std::uint8_t secondary_minimum_quality = 60;
};

struct OverlayPolicy {
    bool startup_status = true;
    std::uint32_t duration_ms = 4000;
};

// Compatibility fields deliberately remain isolated from the new routing model.
// They exist only while the 1.x INI syntax is supported.
struct LegacyCompatibilityPolicy {
    bool force_latencyflex = false;
    std::uint8_t force_reflex = 0;
    std::uint8_t latencyflex_mode = 0;
    bool save_pcl_to_file = false;
    bool legacy_keys_seen = false;
};

struct RuntimePolicySnapshot {
    std::uint64_t generation = 0;
    std::uint64_t routing_signature = 0;
    GeneralPolicy general{};
    InputPolicy input{};
    OutputPolicy output{};
    BackendOrderPolicy backend_order{};
    HybridPolicy hybrid{};
    VulkanPolicy vulkan{};
    VulkanFlexPolicy vulkanflex{};
    OpenXRPolicy openxr{};
    AudioPolicy audio{};
    OverlayPolicy overlay{};
    LegacyCompatibilityPolicy legacy{};
};

[[nodiscard]] std::uint64_t compute_routing_signature(const RuntimePolicySnapshot& snapshot) noexcept;

[[nodiscard]] constexpr std::string_view to_string(LoggingLevel value) noexcept {
    switch (value) {
        case LoggingLevel::Off: return "off";
        case LoggingLevel::Info: return "info";
        case LoggingLevel::Trace: return "trace";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(GraphicsApi value) noexcept {
    switch (value) {
        case GraphicsApi::D3D11: return "d3d11";
        case GraphicsApi::D3D12: return "d3d12";
        case GraphicsApi::Vulkan: return "vulkan";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(InputFrontend value) noexcept {
    switch (value) {
        case InputFrontend::Auto: return "auto";
        case InputFrontend::Reflex: return "reflex";
        case InputFrontend::VkNvLowLatency2: return "vk_nv_low_latency2";
        case InputFrontend::XeLL: return "xell";
        case InputFrontend::AntiLag2: return "antilag2";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(Backend value) noexcept {
    switch (value) {
        case Backend::Auto: return "auto";
        case Backend::AntiLag2: return "antilag2";
        case Backend::XeLL: return "xell";
        case Backend::AmdAntiLagVk: return "amd_anti_lag";
        case Backend::LatencyFlex: return "latencyflex";
        case Backend::NativeReflex: return "native_reflex";
        case Backend::VulkanFlex: return "vulkanflex";
    }
    return "unknown";
}

} // namespace policy
