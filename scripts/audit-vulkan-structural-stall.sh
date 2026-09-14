#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

need() { grep -q -- "$1" "$2" || { echo "missing: $1 in $2" >&2; exit 1; }; }
forbid() { ! grep -q -- "$1" "$2" || { echo "forbidden: $1 in $2" >&2; exit 1; }; }

need 'classify_structural_stall' src/vulkan_structural_stall.cpp
need 'CompensateExternalStall' external/latencyflex.h
need 'hkvkCreateGraphicsPipelines' src/vulkan_hooks.cpp
need 'hkvkCreateComputePipelines' src/vulkan_hooks.cpp
need 'VulkanFlexOnStructuralStall' src/low_latency_vk.cpp
need 'observe_structural_stall' src/low_latency_tech/ll_vulkanflex.cpp
need 'pipeline_threshold_us' src/config.cpp
need 'structural_stall' src/runtime_policy.h

# R4.0 observes pipeline construction but must never rewrite caller pipeline state
# or submit/record GPU work as part of stall handling.
forbid 'vkCmd' src/vulkan_structural_stall.cpp
forbid 'vkQueueSubmit' src/vulkan_structural_stall.cpp
forbid 'vkCreateGraphicsPipelines' src/vulkan_structural_stall.cpp

echo 'audit-vulkan-structural-stall: PASS'
