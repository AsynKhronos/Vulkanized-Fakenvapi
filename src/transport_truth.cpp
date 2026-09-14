#include "transport_truth.h"

namespace transport_truth {
namespace {
std::atomic<std::uint8_t> g_transport{static_cast<std::uint8_t>(ApplicationTransport::Unknown)};
std::atomic<std::uint8_t> g_vulkan_chain{static_cast<std::uint8_t>(VulkanChainState::Unknown)};
std::atomic<std::uint32_t> g_evidence{EvidenceNone};

static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
}

void TransportTruth::reset() noexcept {
    g_transport.store(static_cast<std::uint8_t>(ApplicationTransport::Unknown), std::memory_order_release);
    g_vulkan_chain.store(static_cast<std::uint8_t>(VulkanChainState::Unknown), std::memory_order_release);
    g_evidence.store(EvidenceNone, std::memory_order_release);
}

void TransportTruth::note_translation_bypass_suspected() noexcept {
    g_evidence.fetch_or(EvidenceTranslationHeuristic, std::memory_order_acq_rel);

    auto expected = static_cast<std::uint8_t>(VulkanChainState::Unknown);
    g_vulkan_chain.compare_exchange_strong(
        expected,
        static_cast<std::uint8_t>(VulkanChainState::TranslationBypassSuspected),
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

void TransportTruth::note_native_vulkan_hooks_installed() noexcept {
    g_evidence.fetch_or(EvidenceNativeVulkanHooks, std::memory_order_acq_rel);
    g_vulkan_chain.store(
        static_cast<std::uint8_t>(VulkanChainState::NativeHooksInstalled),
        std::memory_order_release);
}

void TransportTruth::confirm_transport(
    ApplicationTransport candidate,
    std::uint32_t evidence) noexcept {
    g_evidence.fetch_or(evidence, std::memory_order_acq_rel);

    auto current = static_cast<ApplicationTransport>(g_transport.load(std::memory_order_acquire));
    for (;;) {
        ApplicationTransport desired = current;
        if (current == ApplicationTransport::Unknown) {
            desired = candidate;
        } else if (current != candidate && current != ApplicationTransport::Mixed) {
            // Multiple directly-proven transports can legitimately coexist in
            // tools/overlays. Mixed means global truth alone must not suppress a
            // locally proven route; the active LowLatency API becomes decisive.
            desired = ApplicationTransport::Mixed;
        }

        if (desired == current) return;

        auto expected = static_cast<std::uint8_t>(current);
        if (g_transport.compare_exchange_weak(
                expected,
                static_cast<std::uint8_t>(desired),
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
        current = static_cast<ApplicationTransport>(expected);
    }
}

void TransportTruth::confirm_native_vulkan_device() noexcept {
    confirm_transport(ApplicationTransport::NativeVulkan, EvidenceNativeTrackedDevice);
}

void TransportTruth::confirm_vkd3d() noexcept {
    confirm_transport(ApplicationTransport::Vkd3dD3D12, EvidenceVkd3dInterop);
}

void TransportTruth::confirm_dxvk() noexcept {
    confirm_transport(ApplicationTransport::DxvkD3D11, EvidenceDxvkInterop);
}

Snapshot TransportTruth::snapshot() noexcept {
    Snapshot value{};
    value.transport = static_cast<ApplicationTransport>(g_transport.load(std::memory_order_acquire));
    value.vulkan_chain = static_cast<VulkanChainState>(g_vulkan_chain.load(std::memory_order_acquire));
    value.evidence = g_evidence.load(std::memory_order_acquire);
    return value;
}

ApplicationTransport TransportTruth::transport() noexcept {
    return static_cast<ApplicationTransport>(g_transport.load(std::memory_order_acquire));
}

VulkanChainState TransportTruth::vulkan_chain_state() noexcept {
    return static_cast<VulkanChainState>(g_vulkan_chain.load(std::memory_order_acquire));
}

NativeExecutionAccess TransportTruth::native_execution_access() noexcept {
    const auto state = snapshot();

    // A directly confirmed translation transport owns the application route.
    // Mixed is intentionally not blocked: a tool can expose a real native
    // Vulkan device alongside a translation-layer device.
    if (state.transport == ApplicationTransport::Vkd3dD3D12 ||
        state.transport == ApplicationTransport::DxvkD3D11) {
        return NativeExecutionAccess::Blocked;
    }

    if (state.vulkan_chain == VulkanChainState::NativeHooksInstalled)
        return NativeExecutionAccess::FullWsi;

    // If Vulkan detours were conservatively bypassed based on a weak Wine
    // heuristic, explicit NVAPI/Vulkan frame semantics may still be real. Keep
    // the explicit scheduler usable, but disable every feature that assumes WSI
    // ownership/telemetry.
    return NativeExecutionAccess::ExplicitOnly;
}

bool TransportTruth::translation_confirmed() noexcept {
    return has_confirmed_vkd3d() || has_confirmed_dxvk();
}

bool TransportTruth::has_confirmed_native_vulkan() noexcept {
    return (g_evidence.load(std::memory_order_acquire) & EvidenceNativeTrackedDevice) != 0;
}

bool TransportTruth::has_confirmed_vkd3d() noexcept {
    return (g_evidence.load(std::memory_order_acquire) & EvidenceVkd3dInterop) != 0;
}

bool TransportTruth::has_confirmed_dxvk() noexcept {
    return (g_evidence.load(std::memory_order_acquire) & EvidenceDxvkInterop) != 0;
}

} // namespace transport_truth
