#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
header="$root/src/hybrid_fusion.h"
cpp="$root/src/hybrid_fusion.cpp"
tech="$root/src/low_latency_tech/low_latency_tech.h"
ll="$root/src/low_latency_d3d.cpp"
nv="$root/src/fakenvapi.cpp"
vf="$root/src/low_latency_tech/ll_vulkanflex.cpp"

fail() { echo "FAIL: $*" >&2; exit 1; }

grep -q 'struct CanonicalPresentationToken' "$header" || fail "canonical presentation token missing"
grep -q 'render_sequence' "$header" || fail "render/present relation missing"
grep -q 'kPresentationSlots = 128' "$header" || fail "fixed presentation storage missing"
grep -q 'canonicalize_fg_presentation_locked' "$header" || fail "FG canonicalizer missing"
grep -q 'virtual void observe_canonical_presentation' "$tech" || fail "presentation observer ABI missing"
grep -q 'FrontendSetFgType(' "$nv" || fail "OptiScaler FG callback is not routed through frontend arbitration"

fake_fg="$(sed -n '/Fake_InformPresentFG/,/^[[:space:]]*}/p' "$nv")"
grep -q 'InputFrontend::Reflex' <<<"$fake_fg" || fail "Fake_InformPresentFG must identify the Reflex source domain"
if grep -q -- '->set_fg_type' <<<"$fake_fg"; then
  fail "Fake_InformPresentFG still bypasses HybridFusion"
fi

fg_body="$(sed -n '/void LowLatency::FrontendSetFgType(/,/^\/\/ public Reflex frontend/p' "$ll")"
grep -q 'CanonicalPresentationToken' <<<"$fg_body" || fail "FrontendSetFgType does not materialize presentation tokens"
grep -q 'observe_canonical_presentation' <<<"$fg_body" || fail "presentation tokens are not dispatched to telemetry observers"
if grep -Eq 'sleep_with_frame_id|pace_frame|LatencySleep' <<<"$fg_body"; then
  fail "FG metadata path must never introduce a pacing wait"
fi

vf_body="$(sed -n '/void VulkanFlex::observe_canonical_presentation/,/^}/p' "$vf")"
grep -q 'observer_generated_present_count_' <<<"$vf_body" || fail "VulkanFlex does not record generated presentations"
if grep -Eq 'pace_frame|publish_feedback|LatencySleep|sleep_for|Sleep\(' <<<"$vf_body"; then
  fail "VulkanFlex presentation observer became a pacing owner"
fi

grep -q 'presentation_publish_gate_' "$cpp" || fail "fail-open presentation publication gate missing"
grep -q 'next_presentation_sequence_' "$cpp" || fail "independent presentation sequence missing"

echo "PASS: FG render/presentation timelines are independent and observer-only"
