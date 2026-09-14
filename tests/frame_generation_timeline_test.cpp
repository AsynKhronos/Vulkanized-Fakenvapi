#include "hybrid_fusion.h"

#include <cassert>
#include <iostream>

using namespace policy;

int main() {
    RuntimePolicySnapshot snapshot{};
    snapshot.input.minimum_quality = 0;
    snapshot.hybrid.startup_locked = true;
    snapshot.hybrid.startup_observations = 0;
    snapshot.hybrid.aspect_minimum_quality = 0;

    InputArbiter arbiter;
    HybridFusion fusion;
    fusion.set_transport(FrameTransport::Vkd3dD3D12);

    HybridControlPacket al2{};
    al2.enabled = true;
    al2.valid_fields = HybridControlEnabled;
    fusion.publish_control(InputFrontend::AntiLag2, al2);
    fusion.publish_pacing_observation(InputFrontend::AntiLag2);

    constexpr std::uint8_t al2_only = 1u << 3;
    constexpr std::uint64_t render_native_a = 1001;
    arbiter.observe(InputFrontend::AntiLag2, NormalizedMarker::InputSample, render_native_a);

    // Publish FG evidence before startup ownership freezes. No render token is
    // available yet, so the first call is evidence-only.
    assert(!fusion.canonicalize_fg_presentation(
        InputFrontend::AntiLag2, render_native_a, false,
        snapshot, arbiter, al2_only).has_value());
    assert(fusion.locked());
    assert(fusion.source_for_aspect(HybridAspect::FrameGeneration) == InputFrontend::AntiLag2);

    const auto render_a = fusion.canonicalize_pacing_observation(
        InputFrontend::AntiLag2, render_native_a);
    assert(render_a.has_value());

    const auto real_a = fusion.canonicalize_fg_presentation_locked(
        InputFrontend::AntiLag2, render_native_a, false);
    assert(real_a.has_value());
    assert(real_a->render_sequence == render_a->sequence);
    assert(real_a->generation == 1);
    assert(!real_a->interpolated);

    const auto generated_a = fusion.canonicalize_fg_presentation_locked(
        InputFrontend::AntiLag2, render_native_a, true);
    assert(generated_a.has_value());
    assert(generated_a->render_sequence == render_a->sequence);
    assert(generated_a->sequence == real_a->sequence + 1);
    assert(generated_a->generation == 2);
    assert(generated_a->interpolated);

    // The reverse relation remains available from the fixed presentation ring.
    const auto lookup = fusion.lookup_presentation(generated_a->epoch, generated_a->sequence);
    assert(lookup.has_value());
    assert(lookup->render_sequence == render_a->sequence);
    assert(lookup->generation == 2);
    assert(lookup->interpolated);

    constexpr std::uint64_t render_native_b = 1002;
    const auto render_b = fusion.canonicalize_pacing_observation(
        InputFrontend::AntiLag2, render_native_b);
    assert(render_b.has_value());
    assert(render_b->sequence == render_a->sequence + 1);

    const auto real_b = fusion.canonicalize_fg_presentation_locked(
        InputFrontend::AntiLag2, render_native_b, false);
    assert(real_b.has_value());
    assert(real_b->render_sequence == render_b->sequence);
    assert(real_b->generation == 1);
    assert(real_b->sequence == generated_a->sequence + 1);

    // A non-owner cannot create presentation tokens after startup lock.
    assert(!fusion.canonicalize_fg_presentation_locked(
        InputFrontend::Reflex, 77, true).has_value());

    const auto old_epoch = real_b->epoch;
    const auto old_present = real_b->sequence;
    fusion.reset();
    assert(!fusion.lookup_presentation(old_epoch, old_present).has_value());

    std::cout << "PASS: Render/presentation dual timeline and FG 1:N mapping\n";
    return 0;
}
