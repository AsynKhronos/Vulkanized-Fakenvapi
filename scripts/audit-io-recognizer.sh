#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

require() {
    local pattern=$1 file=$2
    if ! grep -Fq "$pattern" "$file"; then
        echo "FAIL missing '$pattern' in ${file#$ROOT/}" >&2
        exit 1
    fi
}

require "enum InputEvidence" "$ROOT/src/input_arbiter.h"
require "InputEvidenceCapability" "$ROOT/src/input_arbiter.h"
require "InputEvidenceContext" "$ROOT/src/input_arbiter.h"
require "InputEvidenceControl" "$ROOT/src/input_arbiter.h"
require "InputEvidenceSleep" "$ROOT/src/input_arbiter.h"
require "InputEvidenceAsyncMarker" "$ROOT/src/input_arbiter.h"
require "InputEvidenceFrameGen" "$ROOT/src/input_arbiter.h"
require "FrontendObserveEvidence" "$ROOT/src/low_latency.h"
require "Input detector frozen" "$ROOT/src/low_latency.cpp"
require "enum class OutputAvailability" "$ROOT/src/output_recognizer.h"
require "OutputAvailability::Unknown" "$ROOT/src/output_recognizer.cpp"
require "recognizer->usable" "$ROOT/src/policy_engine.cpp"
require "detect_d3d_outputs" "$ROOT/src/low_latency_d3d.cpp"
require "probe_vkd3d_vulkan_capabilities" "$ROOT/src/low_latency_d3d.cpp"
require "VK_AMD_ANTI_LAG_EXTENSION_NAME" "$ROOT/src/vkd3d_vulkan_capabilities.cpp"
require "detect_vulkan_outputs" "$ROOT/src/low_latency_vk.cpp"
require "VulkanDeviceRegistry::get_state" "$ROOT/src/low_latency_vk.cpp"

for file in "$ROOT/src/input_arbiter.cpp" "$ROOT/src/output_recognizer.cpp"; do
    if grep -Eq 'std::(vector|map|unordered_map|function)|new[[:space:]]|malloc\(|filesystem' "$file"; then
        echo "FAIL dynamic/heavy primitive found in ${file#$ROOT/}" >&2
        exit 1
    fi
done

"$ROOT/scripts/test-io-recognizer.sh"
echo "PASS: input/output capability recognizer architecture audit"
