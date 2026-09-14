#include "../src/transport_truth.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace transport_truth;

int main() {
    TransportTruth::reset();
    {
        const auto s = TransportTruth::snapshot();
        assert(s.transport == ApplicationTransport::Unknown);
        assert(s.vulkan_chain == VulkanChainState::Unknown);
        assert(s.evidence == EvidenceNone);
        assert(TransportTruth::native_execution_access() == NativeExecutionAccess::ExplicitOnly);
    }

    // Weak Wine/module evidence constrains hook ownership but does not establish
    // application transport. This is the Arknights/OptiScaler false-positive
    // protection that R4.0.1 exists to enforce.
    TransportTruth::note_translation_bypass_suspected();
    {
        const auto s = TransportTruth::snapshot();
        assert(s.transport == ApplicationTransport::Unknown);
        assert(s.vulkan_chain == VulkanChainState::TranslationBypassSuspected);
        assert((s.evidence & EvidenceTranslationHeuristic) != 0);
        assert(!TransportTruth::translation_confirmed());
        assert(TransportTruth::native_execution_access() == NativeExecutionAccess::ExplicitOnly);
    }

    // Public VKD3D interop is authoritative translation evidence and blocks a
    // separate native-Vulkan execution backend when no real native route exists.
    TransportTruth::confirm_vkd3d();
    assert(TransportTruth::transport() == ApplicationTransport::Vkd3dD3D12);
    assert(TransportTruth::has_confirmed_vkd3d());
    assert(TransportTruth::translation_confirmed());
    assert(TransportTruth::native_execution_access() == NativeExecutionAccess::Blocked);

    // Direct Vulkan hook ownership upgrades Native Vulkan to full WSI access.
    TransportTruth::reset();
    TransportTruth::note_native_vulkan_hooks_installed();
    assert(TransportTruth::native_execution_access() == NativeExecutionAccess::FullWsi);
    TransportTruth::confirm_native_vulkan_device();
    assert(TransportTruth::transport() == ApplicationTransport::NativeVulkan);
    assert(TransportTruth::has_confirmed_native_vulkan());
    assert(TransportTruth::native_execution_access() == NativeExecutionAccess::FullWsi);

    // Strong evidence for two transports becomes Mixed instead of allowing one
    // global verdict to erase the other. Local active-API arbitration decides.
    TransportTruth::confirm_vkd3d();
    assert(TransportTruth::transport() == ApplicationTransport::Mixed);
    assert(TransportTruth::has_confirmed_native_vulkan());
    assert(TransportTruth::has_confirmed_vkd3d());
    assert(TransportTruth::native_execution_access() == NativeExecutionAccess::FullWsi);

    TransportTruth::reset();
    TransportTruth::confirm_dxvk();
    assert(TransportTruth::transport() == ApplicationTransport::DxvkD3D11);
    assert(TransportTruth::has_confirmed_dxvk());
    assert(TransportTruth::native_execution_access() == NativeExecutionAccess::Blocked);

    std::cout << "transport truth test: PASS\n";
    return 0;
}
