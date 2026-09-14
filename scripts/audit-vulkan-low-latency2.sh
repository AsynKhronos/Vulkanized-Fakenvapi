#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

hooks="$ROOT/src/vulkan_hooks.cpp"
registry="$ROOT/src/vulkan_device_registry.h"
llvk="$ROOT/src/low_latency_vk.cpp"
main="$ROOT/src/main.cpp"
fakenvapi="$ROOT/src/fakenvapi.cpp"

fail=0

require() {
  local pattern=$1
  local file=$2
  local message=$3
  if ! grep -qE "$pattern" "$file"; then
    echo "FAIL: $message" >&2
    fail=1
  fi
}

require 'hkvkEnumerateDeviceExtensionProperties' "$hooks" 'VK_NV_low_latency2 is not exposed through device-extension enumeration'
require 'VK_NV_LOW_LATENCY_2_EXTENSION_NAME' "$hooks" 'VK_NV_low_latency2 extension name is not handled'
require 'hkvkGetDeviceProcAddr' "$hooks" 'vkGetDeviceProcAddr interception missing'
require 'hkvkGetInstanceProcAddr' "$hooks" 'vkGetInstanceProcAddr interception missing'
require 'hkvkSetLatencySleepModeNV' "$hooks" 'vkSetLatencySleepModeNV frontend missing'
require 'hkvkLatencySleepNV' "$hooks" 'vkLatencySleepNV frontend missing'
require 'hkvkSetLatencyMarkerNV' "$hooks" 'vkSetLatencyMarkerNV frontend missing'
require 'hkvkGetLatencyTimingsNV' "$hooks" 'vkGetLatencyTimingsNV frontend missing'
require 'hkvkQueueNotifyOutOfBandNV' "$hooks" 'vkQueueNotifyOutOfBandNV frontend missing'
require 'InputFrontend::VkNvLowLatency2' "$hooks" 'VK_NV_low_latency2 is not wired into the common input arbiter'
require 'nv_low_latency2_native' "$registry" 'per-device native/emulated VK_NV_low_latency2 state missing'
require 'VulkanFrontendSetMarker' "$llvk" 'shared Vulkan frontend marker bridge missing'
require 'VulkanFrontendObserveMarker' "$llvk" 'native observe-only Vulkan bridge missing'
require 'hkLoadLibrary(A|W|ExA|ExW)' "$hooks" 'delayed Vulkan loader watcher missing'
require 'VulkanHooks::initialize' "$main" 'DllMain does not initialize delayed Vulkan discovery'
require 'VulkanHooks::shutdown' "$main" 'Vulkan detours are not detached on process detach'
require 'detect_existing_vulkan_stack' "$hooks" 'Vulkan coexistence stack classification missing'
require 'module_is_outside_system_directory' "$hooks" 'proxy detection does not distinguish system DLLs from game-local wrappers'
require 'wine_get_version' "$hooks" 'Wine detection for D3D-to-Vulkan translation stacks missing'
require 'Vkd3dProton' "$hooks" 'D3D12+Vulkan vkd3d-proton classification missing'
require 'Dxvk' "$hooks" 'D3D11+Vulkan DXVK classification missing'
require 'optiscaler\.dll' "$hooks" 'explicit OptiScaler module classification missing'
require 'VulkanStackLayer::OptiScalerOrGameProxy' "$hooks" 'OptiScaler layer bit missing'
require 'layers \|= stack_layer_bit\(VulkanStackLayer::Vkd3dProton\)' "$hooks" 'vkd3d layer is not composable with OptiScaler'
require 'layers \|= stack_layer_bit\(VulkanStackLayer::Dxvk\)' "$hooks" 'DXVK layer is not composable with OptiScaler'
require 'OptiScaler/game-local graphics proxy over Wine D3D12\+Vulkan stack' "$hooks" 'layered OptiScaler-over-vkd3d diagnostic missing'
require 'OptiScaler/game-local graphics proxy over Wine D3D11\+Vulkan stack' "$hooks" 'layered OptiScaler-over-DXVK diagnostic missing'
require 'skipping Vulkan core detours' "$hooks" 'auto coexistence path does not explicitly skip conflicting Vulkan detours'
require 'expose_nv_low_latency2 == policy::AutoBool::Enabled' "$hooks" 'explicit VK_NV_low_latency2 force override missing'
require 'nv_low_latency2_emulation_prerequisites_supported' "$hooks" 'VK_NV_low_latency2 emulation dependency gating missing'
require 'VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME' "$hooks" 'timeline-semaphore dependency is not checked'
require 'vkSignalSemaphoreKHR' "$hooks" 'timeline-semaphore KHR dispatch fallback missing for Vulkan 1.1 emulation'
require 'VK_KHR_PRESENT_ID(_2)?_EXTENSION_NAME' "$hooks" 'present-id dependency is not checked'
require 'device groups are not supported' "$hooks" 'VK_NV_low_latency2 device-group rejection missing'
require 'enqueue_emulated_sleep' "$hooks" 'vkLatencySleepNV emulation is not asynchronous'
require 'VulkanFrontendDriveSleep' "$hooks" 'asynchronous sleep worker is not connected to selected Vulkan output'
require 'CreateThread' "$hooks" 'fixed asynchronous Vulkan sleep worker missing'
require 'g_sleep_queue\.reset' "$hooks" 'sleep queue state is not reset during init/shutdown'
require 'if \(sleep_mode_info\)' "$hooks" 'vkSetLatencySleepModeNV NULL-disable semantics missing'
require 'if \(!marker_info->pTimings\)' "$llvk" 'vkGetLatencyTimingsNV count-query semantics missing'
require 'VulkanDeviceRegistry::get_state\(device, &state\)' "$fakenvapi" 'deprecated NVAPI Vulkan low-latency path does not gate unregistered coexistence devices'
require 'return NVAPI_ERROR;' "$fakenvapi" 'unregistered coexistence Vulkan devices do not preserve the legacy NVAPI_ERROR fallback contract'

# Cross-vendor emulation must not pass an unsupported NVIDIA extension down to
# a non-NVIDIA driver. The caller-owned create-info remains immutable; only the
# local extension vector may be filtered.
require 'extensions\.erase' "$hooks" 'emulated VK_NV_low_latency2 is not stripped from the real VkDeviceCreateInfo'
require 'VkDeviceCreateInfo modified_create_info = \*create_info' "$hooks" 'caller-owned VkDeviceCreateInfo is not copied before modification'

if (( fail != 0 )); then exit 1; fi

echo "PASS: VK_NV_low_latency2 Vulkan frontend architecture audit"
