#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRUTH_H="$ROOT/src/transport_truth.h"
TRUTH_CPP="$ROOT/src/transport_truth.cpp"
VK="$ROOT/src/vulkan_hooks.cpp"
LLVK="$ROOT/src/low_latency_vk.cpp"
LLD3D="$ROOT/src/low_latency_d3d.cpp"
VF="$ROOT/src/low_latency_tech/ll_vulkanflex.cpp"
MESON="$ROOT/src/meson.build"

# Weak translation detection may never become authoritative transport by itself.
grep -q 'note_translation_bypass_suspected' "$VK"
grep -q 'TranslationBypassSuspected' "$TRUTH_CPP"
grep -q 'EvidenceTranslationHeuristic' "$TRUTH_CPP"

# Direct proof sources must feed the shared truth plane.
grep -q 'confirm_native_vulkan_device' "$VK"
grep -q 'confirm_vkd3d' "$LLD3D"
grep -q 'confirm_dxvk' "$LLD3D"
grep -q 'confirm_vkd3d' "$VF"
grep -q 'confirm_dxvk' "$VF"

# A suspected/bypassed native route may still use explicit semantics, but it
# must not claim WSI-derived pacing/precision/governor/stall facilities.
grep -q 'native_wsi_authoritative_' "$VF"
grep -q 'native-vulkan-explicit' "$VF"
grep -q 'core WSI/present precision/queue pressure/structural stall disabled' "$VF"
grep -q 'transport_ == Transport::NativeVulkan && native_wsi_authoritative_' "$VF"

# Once D3D translation is proven and active, Vulkan shadow callbacks must not
# switch/deinit the D3D pacing owner or create a second sleep path.
grep -q 'vulkan_frontend_shadowed_by_translation' "$LLVK"
grep -q 'keeping existing D3D pacing owner' "$LLVK"
grep -q 'ignore_shadow_vulkan_frontend("sleep")' "$LLVK"
grep -q 'ignore_shadow_vulkan_frontend("marker")' "$LLVK"

# New state machine must be part of the target build.
grep -q "'transport_truth.cpp'" "$MESON"

# Transport truth itself is control-plane only: no Vulkan/D3D calls or heap.
if grep -Eq 'vkQueueSubmit|vkCmd|new |delete |malloc|free\(' "$TRUTH_H" "$TRUTH_CPP"; then
    echo "authoritative transport truth audit: forbidden execution/heap primitive" >&2
    exit 1
fi

echo "authoritative transport truth audit: PASS"
