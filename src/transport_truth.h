#pragma once

#include <atomic>
#include <cstdint>

namespace transport_truth {

enum class ApplicationTransport : std::uint8_t {
    Unknown = 0,
    NativeVulkan,
    Vkd3dD3D12,
    DxvkD3D11,
    Mixed,
};

enum class VulkanChainState : std::uint8_t {
    Unknown = 0,
    NativeHooksInstalled,
    TranslationBypassSuspected,
};

enum class NativeExecutionAccess : std::uint8_t {
    FullWsi = 0,
    ExplicitOnly,
    Blocked,
};

enum Evidence : std::uint32_t {
    EvidenceNone                      = 0,
    EvidenceTranslationHeuristic      = 1u << 0,
    EvidenceNativeVulkanHooks         = 1u << 1,
    EvidenceNativeTrackedDevice       = 1u << 2,
    EvidenceVkd3dInterop              = 1u << 3,
    EvidenceDxvkInterop               = 1u << 4,
};

struct Snapshot {
    ApplicationTransport transport = ApplicationTransport::Unknown;
    VulkanChainState vulkan_chain = VulkanChainState::Unknown;
    std::uint32_t evidence = EvidenceNone;
};

// Process-lifetime transport truth. Weak loader/module heuristics may constrain
// hook ownership, but only direct native device interception or a public
// translation-layer interop interface can establish authoritative application
// transport. All state is lock-free and control-plane only.
class TransportTruth {
public:
    static void reset() noexcept;

    static void note_translation_bypass_suspected() noexcept;
    static void note_native_vulkan_hooks_installed() noexcept;

    static void confirm_native_vulkan_device() noexcept;
    static void confirm_vkd3d() noexcept;
    static void confirm_dxvk() noexcept;

    [[nodiscard]] static Snapshot snapshot() noexcept;
    [[nodiscard]] static ApplicationTransport transport() noexcept;
    [[nodiscard]] static VulkanChainState vulkan_chain_state() noexcept;
    [[nodiscard]] static NativeExecutionAccess native_execution_access() noexcept;
    [[nodiscard]] static bool translation_confirmed() noexcept;
    [[nodiscard]] static bool has_confirmed_native_vulkan() noexcept;
    [[nodiscard]] static bool has_confirmed_vkd3d() noexcept;
    [[nodiscard]] static bool has_confirmed_dxvk() noexcept;

private:
    static void confirm_transport(ApplicationTransport transport, std::uint32_t evidence) noexcept;
};

[[nodiscard]] constexpr const char* to_string(ApplicationTransport value) noexcept {
    switch (value) {
        case ApplicationTransport::Unknown: return "unknown";
        case ApplicationTransport::NativeVulkan: return "native-vulkan";
        case ApplicationTransport::Vkd3dD3D12: return "vkd3d-d3d12";
        case ApplicationTransport::DxvkD3D11: return "dxvk-d3d11";
        case ApplicationTransport::Mixed: return "mixed";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* to_string(VulkanChainState value) noexcept {
    switch (value) {
        case VulkanChainState::Unknown: return "unknown";
        case VulkanChainState::NativeHooksInstalled: return "native-hooks";
        case VulkanChainState::TranslationBypassSuspected: return "translation-bypass-suspected";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* to_string(NativeExecutionAccess value) noexcept {
    switch (value) {
        case NativeExecutionAccess::FullWsi: return "full-wsi";
        case NativeExecutionAccess::ExplicitOnly: return "explicit-only";
        case NativeExecutionAccess::Blocked: return "blocked";
    }
    return "blocked";
}

} // namespace transport_truth
