#include "policy_engine.h"

namespace policy {

bool backend_supported(GraphicsApi api, Backend backend) noexcept {
    switch (api) {
        case GraphicsApi::D3D11:
            return backend == Backend::VulkanFlex || backend == Backend::AntiLag2 ||
                   backend == Backend::LatencyFlex;
        case GraphicsApi::D3D12:
            return backend == Backend::VulkanFlex || backend == Backend::AmdAntiLagVk ||
                   backend == Backend::AntiLag2 || backend == Backend::XeLL ||
                   backend == Backend::LatencyFlex;
        case GraphicsApi::Vulkan:
            return backend == Backend::VulkanFlex || backend == Backend::AmdAntiLagVk ||
                   backend == Backend::LatencyFlex;
    }
    return false;
}

Backend requested_backend(const RuntimePolicySnapshot& snapshot, GraphicsApi api) noexcept {
    switch (api) {
        case GraphicsApi::D3D11: return snapshot.output.d3d11;
        case GraphicsApi::D3D12: return snapshot.output.d3d12;
        case GraphicsApi::Vulkan: return snapshot.output.vulkan;
    }
    return Backend::Auto;
}

const BackendOrder& fallback_order(const RuntimePolicySnapshot& snapshot, GraphicsApi api) noexcept {
    switch (api) {
        case GraphicsApi::D3D11: return snapshot.backend_order.d3d11;
        case GraphicsApi::D3D12: return snapshot.backend_order.d3d12;
        case GraphicsApi::Vulkan: return snapshot.backend_order.vulkan;
    }
    return snapshot.backend_order.d3d12;
}

BackendCandidates build_backend_candidates(
    const RuntimePolicySnapshot& snapshot,
    GraphicsApi api,
    const OutputRecognizer* recognizer) noexcept {
    BackendCandidates result;
    const Backend explicit_backend = requested_backend(snapshot, api);
    const auto recognized_usable = [&](Backend backend) noexcept {
        return !recognizer || recognizer->usable(backend);
    };
    const auto recognized_available = [&](Backend backend) noexcept {
        return recognizer &&
            recognizer->recognition(backend).availability == OutputAvailability::Available;
    };

    if (explicit_backend != Backend::Auto) {
        if (backend_supported(api, explicit_backend) && recognized_usable(explicit_backend)) {
            result.order.push_back(explicit_backend);
        }
        // Explicit output with fallback disabled is authoritative even when the
        // recognizer proves it unavailable. Return an empty list rather than
        // silently selecting a different backend behind the user's policy.
        if (!snapshot.general.allow_fallback) {
            return result;
        }
    }

    // D3D11/DXVK cooperative execution. VulkanFlex is only auto-promoted when
    // the recognizer proves a usable DXVK bridge. In Auto this means DXVK's own
    // ID3DLowLatencyDevice is the actual pacing owner; VulkanFlex only
    // canonicalizes/orchestrates. Embedded VulkanFlex execution requires the
    // explicit dxvk_execution=enabled policy.
    if (explicit_backend == Backend::Auto && api == GraphicsApi::D3D11 &&
        snapshot.vulkanflex.enabled && snapshot.vulkanflex.dxvk_bridge &&
        snapshot.vulkanflex.dxvk_execution != AutoBool::Disabled &&
        recognized_available(Backend::VulkanFlex)) {
        result.order.push_back(Backend::VulkanFlex);
    }

    // D3D12 low-level execution: when vkd3d-proton exposes its Vulkan interop
    // interface and VK_AMD_anti_lag is enabled on the underlying device, prefer
    // the direct Vulkan backend. Initialization is capability-gated, so native
    // Windows and unsupported GPUs simply fall through with no semantic change.
    // Explicit output choices remain authoritative.
    if (explicit_backend == Backend::Auto && api == GraphicsApi::D3D12 &&
        snapshot.vulkan.prefer_native_extensions && recognized_usable(Backend::AmdAntiLagVk)) {
        result.order.push_back(Backend::AmdAntiLagVk);
    }

    // D3D12 execution policy: XeLL remains the preferred software executor.
    // Its wait is placed at the application frame boundary and should not be
    // duplicated by a second Vulkan-side pacer. VulkanFlex can still attach to
    // VKD3D as an observer/telemetry plane while XeLL owns the only CPU wait.
    if (explicit_backend == Backend::Auto && api == GraphicsApi::D3D12 &&
        snapshot.hybrid.xell_fusion && snapshot.hybrid.startup_locked &&
        recognized_usable(Backend::XeLL)) {
        result.order.push_back(Backend::XeLL);
    }

    // Prefer the native D3D12 Anti-Lag 2 execution path ahead of the generic
    // VulkanFlex bridge when XeLL is unavailable. VulkanFlex remains a safe
    // explicit/fallback executor, but is no longer promoted above mature D3D12
    // pacing implementations merely because VKD3D interop is available.
    if (explicit_backend == Backend::Auto && api == GraphicsApi::D3D12 &&
        recognized_usable(Backend::AntiLag2)) {
        result.order.push_back(Backend::AntiLag2);
    }

    if (explicit_backend == Backend::Auto && api == GraphicsApi::D3D12 &&
        snapshot.vulkanflex.enabled && snapshot.vulkanflex.vkd3d_bridge &&
        snapshot.hybrid.vulkan_fusion && snapshot.hybrid.startup_locked &&
        recognized_available(Backend::VulkanFlex)) {
        result.order.push_back(Backend::VulkanFlex);
    }

    const BackendOrder& configured = fallback_order(snapshot, api);
    for (std::uint8_t i = 0; i < configured.size; ++i) {
        const Backend backend = configured.values[i];
        if (backend == Backend::Auto || !backend_supported(api, backend) ||
            !recognized_usable(backend) || result.order.contains(backend)) {
            continue;
        }
        result.order.push_back(backend);
    }

    return result;
}

} // namespace policy
