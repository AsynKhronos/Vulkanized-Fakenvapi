#include "input_arbiter.h"
#include "policy_engine.h"
#include "output_recognizer.h"
#include "runtime_policy.h"

#include <cassert>
#include <iostream>

using namespace policy;

int main() {
    RuntimePolicySnapshot snapshot{};
    snapshot.routing_signature = compute_routing_signature(snapshot);

    {
        const auto d3d11 = build_backend_candidates(snapshot, GraphicsApi::D3D11);
        assert(d3d11.order.size == 2);
        assert(d3d11.order[0] == Backend::AntiLag2);
        assert(d3d11.order[1] == Backend::LatencyFlex);
    }

    {
        // Proven DXVK low-latency support promotes VulkanFlex as the
        // orchestration backend. Runtime initialization delegates the actual
        // wait to DXVK, so this does not create a second pacer.
        OutputRecognizer outputs;
        outputs.observe(Backend::VulkanFlex, OutputAvailability::Available, 99,
                        OutputEvidenceVulkanInterop | OutputEvidenceDeviceInterface |
                        OutputEvidenceNativeFeature);
        outputs.observe(Backend::AntiLag2, OutputAvailability::Unavailable, 100,
                        OutputEvidenceNone);
        outputs.observe(Backend::LatencyFlex, OutputAvailability::Available, 50,
                        OutputEvidenceSoftwareFallback);
        const auto d3d11 = build_backend_candidates(snapshot, GraphicsApi::D3D11, &outputs);
        assert(d3d11.order.size >= 2);
        assert(d3d11.order[0] == Backend::VulkanFlex);
        assert(d3d11.order[1] == Backend::LatencyFlex);
        assert(backend_supported(GraphicsApi::D3D11, Backend::VulkanFlex));
    }

    {
        auto observer_only = snapshot;
        observer_only.vulkanflex.dxvk_execution = AutoBool::Disabled;
        OutputRecognizer outputs;
        outputs.observe(Backend::VulkanFlex, OutputAvailability::Available, 99,
                        OutputEvidenceVulkanInterop | OutputEvidenceDeviceInterface);
        outputs.observe(Backend::AntiLag2, OutputAvailability::Unavailable, 100,
                        OutputEvidenceNone);
        outputs.observe(Backend::LatencyFlex, OutputAvailability::Available, 50,
                        OutputEvidenceSoftwareFallback);
        const auto d3d11 = build_backend_candidates(observer_only, GraphicsApi::D3D11, &outputs);
        assert(d3d11.order.size == 1);
        assert(d3d11.order[0] == Backend::LatencyFlex);
    }

    {
        const auto d3d12 = build_backend_candidates(snapshot, GraphicsApi::D3D12);
        assert(d3d12.order.size == 4);
        assert(d3d12.order[0] == Backend::AmdAntiLagVk);
        assert(d3d12.order[1] == Backend::XeLL);
        assert(d3d12.order[2] == Backend::AntiLag2);
        assert(d3d12.order[3] == Backend::LatencyFlex);
    }


    {
        auto high_level_first = snapshot;
        high_level_first.vulkan.prefer_native_extensions = false;
        const auto d3d12 = build_backend_candidates(high_level_first, GraphicsApi::D3D12);
        assert(d3d12.order.size == 3);
        assert(d3d12.order[0] == Backend::XeLL);
        assert(d3d12.order[1] == Backend::AntiLag2);
        assert(d3d12.order[2] == Backend::LatencyFlex);
    }

    {
        auto non_hybrid = snapshot;
        non_hybrid.hybrid.xell_fusion = false;
        const auto d3d12 = build_backend_candidates(non_hybrid, GraphicsApi::D3D12);
        assert(d3d12.order.size == 4);
        assert(d3d12.order[0] == Backend::AmdAntiLagVk);
        assert(d3d12.order[1] == Backend::AntiLag2);
        assert(d3d12.order[2] == Backend::XeLL);
        assert(d3d12.order[3] == Backend::LatencyFlex);
    }

    {
        const auto vk = build_backend_candidates(snapshot, GraphicsApi::Vulkan);
        assert(vk.order.size == 3);
        assert(vk.order[0] == Backend::VulkanFlex);
        assert(vk.order[1] == Backend::AmdAntiLagVk);
        assert(vk.order[2] == Backend::LatencyFlex);
    }

    {
        auto explicit_vk = snapshot;
        explicit_vk.output.vulkan = Backend::VulkanFlex;
        explicit_vk.general.allow_fallback = false;
        const auto candidates = build_backend_candidates(explicit_vk, GraphicsApi::Vulkan);
        assert(candidates.order.size == 1);
        assert(candidates.order[0] == Backend::VulkanFlex);
        assert(backend_supported(GraphicsApi::D3D12, Backend::VulkanFlex));
    }

    {
        OutputRecognizer outputs;
        outputs.observe(Backend::VulkanFlex, OutputAvailability::Available, 99,
                        OutputEvidenceVulkanInterop | OutputEvidenceDeviceInterface);
        outputs.observe(Backend::AmdAntiLagVk, OutputAvailability::Unavailable, 100,
                        OutputEvidenceVulkanInterop);
        outputs.observe(Backend::XeLL, OutputAvailability::Available, 95,
                        OutputEvidenceModuleLoaded);
        outputs.observe(Backend::LatencyFlex, OutputAvailability::Available, 50,
                        OutputEvidenceSoftwareFallback);
        const auto candidates = build_backend_candidates(snapshot, GraphicsApi::D3D12, &outputs);
        assert(candidates.order.size >= 3);
        // VKD3D interop no longer promotes VulkanFlex above mature D3D12
        // execution paths. XeLL owns the wait, Anti-Lag 2 remains the next
        // native D3D12 fallback, and VulkanFlex stays available as a bridge
        // executor only if those paths cannot initialize.
        assert(candidates.order[0] == Backend::XeLL);
        assert(candidates.order[1] == Backend::AntiLag2);
        assert(candidates.order[2] == Backend::VulkanFlex);
    }

    {
        auto explicit_policy = snapshot;
        explicit_policy.output.d3d12 = Backend::LatencyFlex;
        explicit_policy.general.allow_fallback = false;
        const auto candidates = build_backend_candidates(explicit_policy, GraphicsApi::D3D12);
        assert(candidates.order.size == 1);
        assert(candidates.order[0] == Backend::LatencyFlex);
    }

    {
        auto explicit_policy = snapshot;
        explicit_policy.output.d3d12 = Backend::XeLL;
        explicit_policy.general.allow_fallback = true;
        const auto candidates = build_backend_candidates(explicit_policy, GraphicsApi::D3D12);
        assert(candidates.order.size == 3);
        assert(candidates.order[0] == Backend::XeLL);
        assert(candidates.order[1] == Backend::AntiLag2);
        assert(candidates.order[2] == Backend::LatencyFlex);
    }

    {
        auto unsupported = snapshot;
        unsupported.output.vulkan = Backend::XeLL;
        unsupported.general.allow_fallback = true;
        const auto candidates = build_backend_candidates(unsupported, GraphicsApi::Vulkan);
        assert(candidates.order.size == 3);
        assert(candidates.order[0] == Backend::VulkanFlex);
        assert(candidates.order[1] == Backend::AmdAntiLagVk);
    }

    {
        InputArbiter arbiter;
        auto input_policy = snapshot;
        input_policy.input.minimum_quality = 45;

        for (std::uint64_t frame = 1; frame <= 4; ++frame) {
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, frame);
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::RenderSubmitStart, frame);
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::PresentStart, frame);
        }

        const auto selected = arbiter.select(input_policy);
        assert(selected.has_value());
        assert(*selected == InputFrontend::Reflex);
        assert(arbiter.quality(InputFrontend::Reflex) >= input_policy.input.minimum_quality);

        // Freshness advances once per native frame, not once per marker. A
        // marker-rich frontend must not age sparse sources faster merely
        // because it exposes a denser marker vocabulary.
        assert(arbiter.observation_sequence() == 4);
    }

    {
        InputArbiter arbiter;
        auto explicit_input = snapshot;
        explicit_input.input.mode = InputFrontend::XeLL;
        explicit_input.input.minimum_quality = 30;
        explicit_input.general.allow_fallback = false;
        arbiter.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, 1);
        arbiter.observe(InputFrontend::Reflex, NormalizedMarker::PresentStart, 1);
        assert(!arbiter.select(explicit_input).has_value());
    }


    {
        // Callback density is not quality. Repeating the same marker within one
        // native frame must neither advance freshness nor inflate recognition.
        InputArbiter sparse;
        InputArbiter noisy;
        sparse.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, 1);
        noisy.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, 1);
        const auto baseline_quality = sparse.quality(InputFrontend::Reflex);
        for (int i = 0; i < 64; ++i)
            noisy.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, 1);
        assert(noisy.observation_sequence() == 1);
        assert(noisy.quality(InputFrontend::Reflex) == baseline_quality);
    }

    {
        // Marker density must not change the long-run reliability ramp. One
        // frontend emits only SimulationStart; the other emits the same frame
        // plus five additional callbacks. With identical native frame progress
        // both must end at identical quality.
        InputArbiter sparse;
        InputArbiter verbose;
        for (std::uint64_t frame = 1; frame <= 12; ++frame) {
            sparse.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, frame);

            verbose.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, frame);
            verbose.observe(InputFrontend::Reflex, NormalizedMarker::RenderSubmitStart, frame);
            verbose.observe(InputFrontend::Reflex, NormalizedMarker::RenderSubmitEnd, frame);
            verbose.observe(InputFrontend::Reflex, NormalizedMarker::PresentStart, frame);
            verbose.observe(InputFrontend::Reflex, NormalizedMarker::PresentEnd, frame);
            verbose.observe(InputFrontend::Reflex, NormalizedMarker::TriggerFlash, frame);
        }

        // Vocabulary may legitimately contribute one-time quality evidence, so
        // compare two sources with the same learned vocabulary after warm-up.
        InputArbiter sparse_vocab;
        InputArbiter verbose_vocab;
        constexpr NormalizedMarker vocabulary[] = {
            NormalizedMarker::SimulationStart,
            NormalizedMarker::RenderSubmitStart,
            NormalizedMarker::RenderSubmitEnd,
            NormalizedMarker::PresentStart,
            NormalizedMarker::PresentEnd,
            NormalizedMarker::TriggerFlash,
        };
        for (auto marker : vocabulary) {
            sparse_vocab.observe(InputFrontend::Reflex, marker, 1);
            verbose_vocab.observe(InputFrontend::Reflex, marker, 1);
        }
        for (std::uint64_t frame = 2; frame <= 12; ++frame) {
            sparse_vocab.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, frame);
            for (auto marker : vocabulary)
                verbose_vocab.observe(InputFrontend::Reflex, marker, frame);
        }
        assert(sparse_vocab.observation_sequence() == 12);
        assert(verbose_vocab.observation_sequence() == 12);
        assert(sparse_vocab.quality(InputFrontend::Reflex) ==
               verbose_vocab.quality(InputFrontend::Reflex));
    }

    {
        // A previously strong source must not remain selected forever after it
        // stops producing markers and another frontend becomes active.
        InputArbiter arbiter;
        auto input_policy = snapshot;
        input_policy.input.minimum_quality = 45;

        for (std::uint64_t frame = 1; frame <= 8; ++frame) {
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, frame);
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::RenderSubmitStart, frame);
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::PresentStart, frame);
        }
        auto selected = arbiter.select(input_policy);
        assert(selected.has_value() && *selected == InputFrontend::Reflex);

        for (std::uint64_t frame = 1; frame <= 40; ++frame) {
            arbiter.observe(InputFrontend::XeLL, NormalizedMarker::SimulationStart, frame);
            arbiter.observe(InputFrontend::XeLL, NormalizedMarker::RenderSubmitStart, frame);
            arbiter.observe(InputFrontend::XeLL, NormalizedMarker::PresentStart, frame);
        }
        selected = arbiter.select(input_policy, InputFrontend::Reflex);
        assert(selected.has_value() && *selected == InputFrontend::XeLL);
        assert(!arbiter.fresh(InputFrontend::Reflex));
    }

    {
        // Once two healthy frontends converge to equal quality, the current
        // source stays selected. Priority is only an initial tie-breaker; it
        // must not cause runtime oscillation between equal-quality sources.
        InputArbiter arbiter;
        auto input_policy = snapshot;
        input_policy.input.minimum_quality = 45;

        for (std::uint64_t frame = 1; frame <= 20; ++frame) {
            arbiter.observe(InputFrontend::XeLL, NormalizedMarker::SimulationStart, frame);
            arbiter.observe(InputFrontend::XeLL, NormalizedMarker::RenderSubmitStart, frame);
            arbiter.observe(InputFrontend::XeLL, NormalizedMarker::PresentStart, frame);
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, frame);
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::RenderSubmitStart, frame);
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::PresentStart, frame);
        }

        assert(arbiter.quality(InputFrontend::XeLL) == 100);
        assert(arbiter.quality(InputFrontend::Reflex) == 100);
        const auto selected = arbiter.select(input_policy, InputFrontend::XeLL);
        assert(selected.has_value() && *selected == InputFrontend::XeLL);
    }

    {
        // Explicitly disabled frontends remain observable but cannot win.
        InputArbiter arbiter;
        auto input_policy = snapshot;
        input_policy.input.minimum_quality = 45;

        for (std::uint64_t frame = 1; frame <= 20; ++frame) {
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, frame);
            arbiter.observe(InputFrontend::AntiLag2, NormalizedMarker::SimulationStart, frame);
            arbiter.observe(InputFrontend::Reflex, NormalizedMarker::PresentStart, frame);
            arbiter.observe(InputFrontend::AntiLag2, NormalizedMarker::PresentStart, frame);
        }

        assert(arbiter.quality(InputFrontend::Reflex) == 100);
        assert(arbiter.quality(InputFrontend::AntiLag2) == 100);
        constexpr std::uint8_t only_antilag2 = 1u << 3;
        const auto selected = arbiter.select(input_policy, InputFrontend::Reflex, only_antilag2);
        assert(selected.has_value() && *selected == InputFrontend::AntiLag2);
    }


    {
        // Output recognition is tri-state. A proven-unavailable direct backend
        // is filtered, while Unknown fallbacks remain eligible so recognition
        // can never destroy a path merely because probing was incomplete.
        OutputRecognizer outputs;
        outputs.observe(Backend::AmdAntiLagVk, OutputAvailability::Unavailable, 100,
                        OutputEvidenceVulkanInterop);
        outputs.observe(Backend::XeLL, OutputAvailability::Available, 80,
                        OutputEvidenceLibraryPresent);
        outputs.observe(Backend::LatencyFlex, OutputAvailability::Available, 50,
                        OutputEvidenceSoftwareFallback);

        const auto candidates = build_backend_candidates(snapshot, GraphicsApi::D3D12, &outputs);
        assert(candidates.order.size >= 2);
        assert(candidates.order[0] == Backend::XeLL);
        assert(!candidates.order.contains(Backend::AmdAntiLagVk));
        assert(candidates.order.contains(Backend::AntiLag2)); // Unknown remains usable.
    }

    {
        OutputRecognizer outputs;
        outputs.observe(Backend::AmdAntiLagVk, OutputAvailability::Unknown, 70,
                        OutputEvidenceVulkanInterop);
        outputs.observe(Backend::AmdAntiLagVk, OutputAvailability::Available, 98,
                        OutputEvidenceNativeExtension | OutputEvidenceNativeFeature);
        const auto result = outputs.recognition(Backend::AmdAntiLagVk);
        assert(result.availability == OutputAvailability::Available);
        assert(result.confidence == 98);
        assert((result.evidence & OutputEvidenceVulkanInterop) != 0);
        assert((result.evidence & OutputEvidenceNativeFeature) != 0);
    }

    {
        // Input recognition now distinguishes capability/context/control/sleep
        // evidence from marker evidence. Presence alone does not become fresh
        // and therefore cannot win arbitration without frame activity.
        InputArbiter arbiter;
        arbiter.note_evidence(InputFrontend::XeLL, InputEvidenceContext);
        arbiter.note_evidence(InputFrontend::XeLL, InputEvidenceControl);
        auto recognized = arbiter.recognition(InputFrontend::XeLL);
        assert((recognized.evidence_mask & InputEvidenceContext) != 0);
        assert((recognized.evidence_mask & InputEvidenceControl) != 0);
        assert(!recognized.fresh);

        arbiter.note_evidence(InputFrontend::XeLL, InputEvidenceSleep);
        arbiter.observe(InputFrontend::XeLL, NormalizedMarker::SimulationStart, 1);
        recognized = arbiter.recognition(InputFrontend::XeLL);
        assert(recognized.fresh);
        assert((recognized.evidence_mask & InputEvidenceSleep) != 0);
        assert((recognized.evidence_mask & InputEvidenceMarker) != 0);
        assert(recognized.quality >= 45);
    }

    {
        const auto base_signature = snapshot.routing_signature;
        auto logging_only = snapshot;
        logging_only.general.logging = LoggingLevel::Trace;
        assert(compute_routing_signature(logging_only) == base_signature);

        auto routing_change = snapshot;
        routing_change.output.vulkan = Backend::LatencyFlex;
        assert(compute_routing_signature(routing_change) != base_signature);

        auto hybrid_change = snapshot;
        hybrid_change.hybrid.xell_fusion = false;
        assert(compute_routing_signature(hybrid_change) != base_signature);

        auto structural_change = snapshot;
        structural_change.vulkanflex.structural_stall = AutoBool::Disabled;
        assert(compute_routing_signature(structural_change) != base_signature);
    }

    std::cout << "PASS: policy engine and input arbiter\n";
    return 0;
}
