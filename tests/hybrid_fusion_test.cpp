#include "hybrid_fusion.h"

#include <cassert>
#include <iostream>
#include <atomic>
#include <thread>

using namespace policy;

static void publish_controls(HybridFusion& fusion) {
    HybridControlPacket al2{};
    al2.enabled = true;
    al2.minimum_interval_us = 8333;
    al2.valid_fields = HybridControlEnabled | HybridControlMinimumInterval;
    fusion.publish_control(InputFrontend::AntiLag2, al2);

    HybridControlPacket reflex{};
    reflex.enabled = true;
    reflex.boost = true;
    reflex.minimum_interval_us = 5000;
    reflex.use_markers_to_optimize = true;
    reflex.valid_fields = HybridControlEnabled | HybridControlBoost |
                          HybridControlMinimumInterval | HybridControlMarkers;
    fusion.publish_control(InputFrontend::Reflex, reflex);

    HybridControlPacket xell{};
    xell.enabled = true;
    xell.boost = false;
    xell.minimum_interval_us = 4000;
    xell.valid_fields = HybridControlEnabled | HybridControlBoost |
                        HybridControlMinimumInterval;
    fusion.publish_control(InputFrontend::XeLL, xell);

    fusion.publish_pacing_observation(InputFrontend::AntiLag2);
    fusion.publish_pacing_observation(InputFrontend::Reflex);
    fusion.publish_pacing_observation(InputFrontend::XeLL);
}

static void observe_frame(InputArbiter& arbiter, std::uint64_t frame) {
    constexpr InputFrontend frontends[] = {
        InputFrontend::AntiLag2,
        InputFrontend::Reflex,
        InputFrontend::XeLL,
    };
    for (const auto frontend : frontends) {
        arbiter.observe(frontend, NormalizedMarker::SimulationStart, frame);
        arbiter.observe(frontend, NormalizedMarker::SimulationEnd, frame);
        arbiter.observe(frontend, NormalizedMarker::RenderSubmitStart, frame);
        arbiter.observe(frontend, NormalizedMarker::RenderSubmitEnd, frame);
        arbiter.observe(frontend, NormalizedMarker::PresentStart, frame);
        arbiter.observe(frontend, NormalizedMarker::PresentEnd, frame);
        arbiter.observe(frontend, NormalizedMarker::InputSample, frame);
    }
    // Reflex/VK-LL2 style latency instrumentation can expose these signals;
    // XeLL and AL2 do not. Keep them independent from InputSample ownership.
    arbiter.observe(InputFrontend::Reflex, NormalizedMarker::TriggerFlash, frame);
    arbiter.observe(InputFrontend::Reflex, NormalizedMarker::PcLatencyPing, frame);
}

int main() {
    RuntimePolicySnapshot snapshot{};
    snapshot.input.minimum_quality = 45;
    snapshot.hybrid.xell_fusion = true;
    snapshot.hybrid.startup_locked = true;
    snapshot.hybrid.startup_observations = 64;
    snapshot.hybrid.aspect_minimum_quality = 45;

    InputArbiter arbiter;
    HybridFusion fusion;
    fusion.set_transport(FrameTransport::Vkd3dD3D12);
    publish_controls(fusion);

    constexpr std::uint8_t all_eligible = 0x0f;

    // Warmup is observe-only. The hybrid must not choose a provisional pacing
    // source and must not permit XeLL Sleep() before the startup decision locks.
    for (std::uint64_t frame = 1; frame <= 3; ++frame) {
        observe_frame(arbiter, frame);
        assert(!fusion.resolve_pacing_source(snapshot, arbiter, all_eligible).has_value());
        assert(!fusion.locked());
    }

    // Keep feeding startup evidence. resolve_pacing_source() is called as real
    // callbacks arrive and freezes the entire per-aspect ownership table once.
    std::optional<InputFrontend> pacing;
    // Opt7 startup observations are frame-activity observations, not raw
    // marker callbacks. With three active frontends the 64-observation window
    // therefore needs a little over 21 native frames instead of being
    // accelerated by the seven-marker vocabulary above.
    for (std::uint64_t frame = 4; frame <= 32 && !fusion.locked(); ++frame) {
        observe_frame(arbiter, frame);
        // Expose AL2 FG capability before the one-time lock.
        (void)fusion.publish_fg_type(InputFrontend::AntiLag2, frame, true,
                                     snapshot, arbiter, all_eligible);
        pacing = fusion.resolve_pacing_source(snapshot, arbiter, all_eligible);
    }

    assert(fusion.locked());
    pacing = fusion.resolve_pacing_source(snapshot, arbiter, all_eligible);
    assert(pacing.has_value());
    assert(*pacing == InputFrontend::AntiLag2);

    const auto resolved = fusion.resolve_control(snapshot, arbiter, all_eligible);
    assert(resolved.has_value());

    // Per-aspect winners are selected once from startup evidence.
    assert(resolved->pacing_source == InputFrontend::AntiLag2);
    assert(resolved->enabled_source == InputFrontend::Reflex);
    assert(resolved->value.enabled);
    assert(resolved->boost_source == InputFrontend::Reflex);
    assert(resolved->value.boost);
    assert(resolved->interval_source == InputFrontend::XeLL);
    assert(resolved->value.minimum_interval_us == 4000);
    assert(resolved->markers_source == InputFrontend::Reflex);
    assert(resolved->value.use_markers_to_optimize);
    assert(fusion.source_for_aspect(HybridAspect::InputSampling) == InputFrontend::AntiLag2);
    assert(fusion.source_for_aspect(HybridAspect::TriggerFlash) == InputFrontend::Reflex);
    assert(fusion.source_for_aspect(HybridAspect::PcLatencyPing) == InputFrontend::Reflex);

    // Opt4: a control update is one coherent frontend generation. The reader
    // may observe A or B, but never Enabled from A with Boost/Markers from B.
    {
        HybridControlPacket control_a{};
        control_a.enabled = true;
        control_a.boost = true;
        control_a.minimum_interval_us = 5000;
        control_a.use_markers_to_optimize = true;
        control_a.valid_fields = HybridControlEnabled | HybridControlBoost |
                                 HybridControlMinimumInterval | HybridControlMarkers;
        HybridControlPacket control_b = control_a;
        control_b.enabled = false;
        control_b.boost = false;
        control_b.use_markers_to_optimize = false;
        control_b.minimum_interval_us = 9000;

        std::atomic<bool> writer_done{false};
        std::thread writer([&] {
            for (int i = 0; i < 20000; ++i)
                fusion.publish_control(InputFrontend::Reflex, (i & 1) ? control_a : control_b);
            writer_done.store(true, std::memory_order_release);
        });

        do {
            const auto concurrent = fusion.resolve_control(snapshot, arbiter, all_eligible);
            assert(concurrent.has_value());
            if (concurrent->enabled_source == InputFrontend::Reflex &&
                concurrent->boost_source == InputFrontend::Reflex &&
                concurrent->markers_source == InputFrontend::Reflex) {
                const bool e = concurrent->value.enabled;
                assert(concurrent->value.boost == e);
                assert(concurrent->value.use_markers_to_optimize == e);
            }
        } while (!writer_done.load(std::memory_order_acquire));
        writer.join();
        fusion.publish_control(InputFrontend::Reflex, control_a);
    }

    // Runtime quality/eligibility changes are intentionally ignored after lock.
    // The ownership map is immutable until HybridFusion::reset()/backend deinit.
    constexpr std::uint8_t al2_xell_only = (1u << 2) | (1u << 3);
    for (std::uint64_t frame = 100; frame < 220; ++frame) {
        arbiter.observe(InputFrontend::XeLL, NormalizedMarker::SimulationStart, frame);
        arbiter.observe(InputFrontend::AntiLag2, NormalizedMarker::InputSample, frame);
    }
    const auto still_locked = fusion.resolve_control(snapshot, arbiter, al2_xell_only);
    assert(still_locked.has_value());
    assert(still_locked->pacing_source == InputFrontend::AntiLag2);
    assert(still_locked->boost_source == InputFrontend::Reflex);
    assert(still_locked->interval_source == InputFrontend::XeLL);

    // Exact-frame marker routing follows the startup-frozen aspect owners.
    constexpr std::uint64_t frame = 500;
    arbiter.observe(InputFrontend::AntiLag2, NormalizedMarker::SimulationStart, frame);
    arbiter.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, frame);
    arbiter.observe(InputFrontend::XeLL, NormalizedMarker::SimulationStart, frame);

    // Pacing owner establishes the canonical frame, but Reflex owns Simulation.
    assert(!fusion.accept_marker(InputFrontend::AntiLag2,
                                 NormalizedMarker::SimulationStart, frame,
                                 snapshot, arbiter, all_eligible));
    assert(fusion.accept_marker(InputFrontend::Reflex,
                                NormalizedMarker::SimulationStart, frame,
                                snapshot, arbiter, all_eligible));
    assert(!fusion.accept_marker(InputFrontend::XeLL,
                                 NormalizedMarker::SimulationStart, frame,
                                 snapshot, arbiter, all_eligible));

    // Low-level execution can anchor a canonical frame directly from the frozen
    // pacing owner's Sleep callback before SimulationStart arrives. This is
    // required by VK_AMD_anti_lag, where INPUT is the earliest useful stage.
    constexpr std::uint64_t early_frame = 700;
    fusion.publish_pacing_observation(InputFrontend::AntiLag2, early_frame);
    assert(fusion.accept_marker(InputFrontend::AntiLag2,
                                NormalizedMarker::InputSample, early_frame,
                                snapshot, arbiter, all_eligible));
    assert(!fusion.accept_marker(InputFrontend::Reflex,
                                 NormalizedMarker::InputSample, early_frame,
                                 snapshot, arbiter, all_eligible));
    assert(fusion.accept_marker(InputFrontend::Reflex,
                                NormalizedMarker::TriggerFlash, early_frame,
                                snapshot, arbiter, all_eligible));
    assert(fusion.accept_marker(InputFrontend::Reflex,
                                NormalizedMarker::PcLatencyPing, early_frame,
                                snapshot, arbiter, all_eligible));

    // R3.4: frontend-native frame IDs are independent domains. The frozen
    // pacing source creates one internal token, while Reflex can bind a totally
    // different native ID to the same frame. Every accepted semantic marker
    // must resolve to that same monotonic execution sequence.
    constexpr std::uint64_t al2_native_a = 90001;
    constexpr std::uint64_t reflex_native_a = 502;
    const auto token_a = fusion.canonicalize_pacing_observation(
        InputFrontend::AntiLag2, al2_native_a);
    assert(token_a.has_value());
    assert(token_a->source_frame_id == al2_native_a);
    assert(token_a->transport == FrameTransport::Vkd3dD3D12);

    const auto sim_a = fusion.canonicalize_marker_locked(
        InputFrontend::Reflex, NormalizedMarker::SimulationStart, reflex_native_a);
    assert(sim_a.has_value());
    assert(sim_a->sequence == token_a->sequence);
    assert(sim_a->epoch == token_a->epoch);
    assert(sim_a->source_frame_id == reflex_native_a);

    const auto present_a = fusion.canonicalize_marker_locked(
        InputFrontend::Reflex, NormalizedMarker::PresentStart, reflex_native_a);
    assert(present_a.has_value());
    assert(present_a->sequence == token_a->sequence);

    // The next pacing frame gets a new internal sequence even if each frontend
    // uses its own unrelated numbering scheme. A delayed marker carrying the
    // old Reflex ID still resolves to the old token via the fixed O(1) binding.
    constexpr std::uint64_t al2_native_b = 90002;
    constexpr std::uint64_t reflex_native_b = 777;
    const auto token_b = fusion.canonicalize_pacing_observation(
        InputFrontend::AntiLag2, al2_native_b);
    assert(token_b.has_value());
    assert(token_b->sequence == token_a->sequence + 1);

    const auto sim_b = fusion.canonicalize_marker_locked(
        InputFrontend::Reflex, NormalizedMarker::SimulationStart, reflex_native_b);
    assert(sim_b.has_value());
    assert(sim_b->sequence == token_b->sequence);

    const auto delayed_old = fusion.canonicalize_marker_locked(
        InputFrontend::Reflex, NormalizedMarker::PresentEnd, reflex_native_a);
    assert(delayed_old.has_value());
    assert(delayed_old->sequence == token_a->sequence);

    // Opt4: if the pacing source gets two frames ahead, a newly seen delayed
    // secondary ID must not be attached to the newest token. Native-domain
    // delta 1 maps to the first missing canonical token; delta 2 then catches
    // up to the current token without comparing IDs across frontends.
    constexpr std::uint64_t al2_native_c = 90003;
    constexpr std::uint64_t al2_native_d = 90004;
    const auto token_c = fusion.canonicalize_pacing_observation(
        InputFrontend::AntiLag2, al2_native_c);
    const auto token_d = fusion.canonicalize_pacing_observation(
        InputFrontend::AntiLag2, al2_native_d);
    assert(token_c.has_value() && token_d.has_value());
    assert(token_d->sequence == token_c->sequence + 1);

    const auto delayed_c = fusion.canonicalize_marker_locked(
        InputFrontend::Reflex, NormalizedMarker::SimulationStart, reflex_native_b + 1);
    assert(delayed_c.has_value());
    assert(delayed_c->sequence == token_c->sequence);

    const auto catchup_d = fusion.canonicalize_marker_locked(
        InputFrontend::Reflex, NormalizedMarker::PresentStart, reflex_native_b + 2);
    assert(catchup_d.has_value());
    assert(catchup_d->sequence == token_d->sequence);

    // Opt4: lifecycle ownership requires the complete semantic pair. A very
    // high-quality source that only exposes SimulationStart must not freeze as
    // owner of SimulationEnd as well. Reflex provides both markers and wins the
    // lifecycle even when Anti-Lag 2 has accumulated more generic quality.
    {
        RuntimePolicySnapshot integrity_snapshot{};
        integrity_snapshot.hybrid.startup_locked = true;
        integrity_snapshot.hybrid.startup_observations = 8;
        integrity_snapshot.hybrid.aspect_minimum_quality = 20;

        InputArbiter integrity_arbiter;
        HybridFusion integrity_fusion;
        HybridControlPacket enabled{};
        enabled.enabled = true;
        enabled.valid_fields = HybridControlEnabled;
        integrity_fusion.publish_control(InputFrontend::AntiLag2, enabled);
        integrity_fusion.publish_control(InputFrontend::Reflex, enabled);
        integrity_fusion.publish_pacing_observation(InputFrontend::AntiLag2);
        integrity_fusion.publish_pacing_observation(InputFrontend::Reflex);

        for (std::uint64_t f = 1; f <= 20 && !integrity_fusion.locked(); ++f) {
            // Inflate AL2 quality without ever supplying SimulationEnd.
            integrity_arbiter.observe(InputFrontend::AntiLag2, NormalizedMarker::SimulationStart, f);
            integrity_arbiter.observe(InputFrontend::AntiLag2, NormalizedMarker::InputSample, f);
            integrity_arbiter.observe(InputFrontend::AntiLag2, NormalizedMarker::RenderSubmitEnd, f);
            integrity_arbiter.observe(InputFrontend::Reflex, NormalizedMarker::SimulationStart, f);
            integrity_arbiter.observe(InputFrontend::Reflex, NormalizedMarker::SimulationEnd, f);
            (void)integrity_fusion.resolve_pacing_source(
                integrity_snapshot, integrity_arbiter, all_eligible);
        }
        assert(integrity_fusion.locked());
        const auto lifecycle_owner = integrity_fusion.source_for_aspect(
            HybridAspect::SimulationLifecycle);
        assert(lifecycle_owner.has_value());
        assert(*lifecycle_owner == InputFrontend::Reflex);
    }

    std::cout << "PASS: Startup-locked per-aspect Hybrid low-level arbitration\n";
    return 0;
}
