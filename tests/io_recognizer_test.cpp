#include "input_arbiter.h"
#include "output_recognizer.h"
#include "policy_engine.h"
#include "runtime_policy.h"

#include <cassert>
#include <iostream>

using namespace policy;

int main() {
    RuntimePolicySnapshot snapshot{};
    snapshot.input.minimum_quality = 45;

    {
        InputArbiter detector;
        detector.note_evidence(InputFrontend::XeLL, InputEvidenceCapability);
        detector.note_evidence(InputFrontend::XeLL, InputEvidenceContext);
        detector.note_evidence(InputFrontend::XeLL, InputEvidenceControl);

        const auto passive = detector.recognition(InputFrontend::XeLL);
        assert(!passive.fresh);
        assert((passive.evidence_mask & InputEvidenceCapability) != 0);
        assert((passive.evidence_mask & InputEvidenceContext) != 0);
        assert((passive.evidence_mask & InputEvidenceControl) != 0);
        assert(!detector.select(snapshot).has_value());

        detector.note_evidence(InputFrontend::XeLL, InputEvidenceSleep);
        detector.observe(InputFrontend::XeLL, NormalizedMarker::SimulationStart, 100);
        detector.observe(InputFrontend::XeLL, NormalizedMarker::RenderSubmitStart, 100);
        detector.observe(InputFrontend::XeLL, NormalizedMarker::PresentStart, 100);

        const auto active = detector.recognition(InputFrontend::XeLL);
        assert(active.fresh);
        assert(active.quality >= snapshot.input.minimum_quality);
        assert((active.evidence_mask & InputEvidenceSleep) != 0);
        assert((active.evidence_mask & InputEvidenceMarker) != 0);
        const auto selected = detector.select(snapshot);
        assert(selected.has_value() && *selected == InputFrontend::XeLL);
    }

    {
        OutputRecognizer detector;
        detector.observe(Backend::AmdAntiLagVk, OutputAvailability::Unavailable, 100,
                         OutputEvidenceVulkanInterop);
        detector.observe(Backend::XeLL, OutputAvailability::Available, 85,
                         OutputEvidenceLibraryPresent);
        detector.observe(Backend::LatencyFlex, OutputAvailability::Available, 50,
                         OutputEvidenceSoftwareFallback);

        // AntiLag2 remains Unknown and must therefore still be tryable.
        const auto candidates = build_backend_candidates(snapshot, GraphicsApi::D3D12, &detector);
        assert(!candidates.order.contains(Backend::AmdAntiLagVk));
        assert(candidates.order.size >= 2);
        assert(candidates.order[0] == Backend::XeLL);
        assert(candidates.order.contains(Backend::AntiLag2));
    }

    {
        auto explicit_policy = snapshot;
        explicit_policy.output.d3d12 = Backend::AmdAntiLagVk;
        explicit_policy.general.allow_fallback = false;

        OutputRecognizer detector;
        detector.observe(Backend::AmdAntiLagVk, OutputAvailability::Unavailable, 100,
                         OutputEvidenceVulkanInterop);
        detector.observe(Backend::XeLL, OutputAvailability::Available, 90,
                         OutputEvidenceModuleLoaded);

        const auto candidates = build_backend_candidates(
            explicit_policy, GraphicsApi::D3D12, &detector);
        assert(candidates.order.size == 0);
    }

    {
        OutputRecognizer detector;
        detector.observe(Backend::AmdAntiLagVk, OutputAvailability::Unknown, 70,
                         OutputEvidenceVulkanInterop);
        detector.observe(Backend::AmdAntiLagVk, OutputAvailability::Available, 98,
                         OutputEvidenceNativeExtension | OutputEvidenceNativeFeature);
        const auto recognized = detector.recognition(Backend::AmdAntiLagVk);
        assert(recognized.availability == OutputAvailability::Available);
        assert(recognized.confidence == 98);
        assert((recognized.evidence & OutputEvidenceVulkanInterop) != 0);
        assert((recognized.evidence & OutputEvidenceNativeFeature) != 0);
    }

    std::cout << "PASS: input/output capability recognizer\n";
    return 0;
}
