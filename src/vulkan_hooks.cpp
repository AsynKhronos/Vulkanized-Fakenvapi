#include "vulkan_hooks.h"

#include "config.h"
#include "fakenvapi.h"
#include "fixed_mpmc_ring.h"
#include "vulkan_device_registry.h"
#include "transport_truth.h"
#include "openxr_hooks.h"
#include "vulkan_capabilities.h"
#include "vulkan_structural_stall.h"
#include "util.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

PFN_vkGetInstanceProcAddr VulkanHooks::o_vkGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr VulkanHooks::o_vkGetDeviceProcAddr = nullptr;
PFN_vkCreateDevice VulkanHooks::o_vkCreateDevice = nullptr;
PFN_vkDestroyDevice VulkanHooks::o_vkDestroyDevice = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 VulkanHooks::o_vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkGetPhysicalDeviceProperties VulkanHooks::o_vkGetPhysicalDeviceProperties = nullptr;
PFN_vkEnumerateDeviceExtensionProperties VulkanHooks::o_vkEnumerateDeviceExtensionProperties = nullptr;
PFN_vkGetDeviceQueue VulkanHooks::o_vkGetDeviceQueue = nullptr;
PFN_vkGetDeviceQueue2 VulkanHooks::o_vkGetDeviceQueue2 = nullptr;
PFN_vkAcquireNextImageKHR VulkanHooks::o_vkAcquireNextImageKHR = nullptr;
PFN_vkAcquireNextImage2KHR VulkanHooks::o_vkAcquireNextImage2KHR = nullptr;
PFN_vkCreateSwapchainKHR VulkanHooks::o_vkCreateSwapchainKHR = nullptr;
PFN_vkQueuePresentKHR VulkanHooks::o_vkQueuePresentKHR = nullptr;
PFN_vkDestroySwapchainKHR VulkanHooks::o_vkDestroySwapchainKHR = nullptr;

namespace {
std::mutex g_hook_mutex;
HMODULE g_hooked_vulkan_module = nullptr;
bool g_vulkan_hooks_installed = false;

using LoadLibraryAFn = HMODULE (WINAPI*)(LPCSTR);
using LoadLibraryWFn = HMODULE (WINAPI*)(LPCWSTR);
using LoadLibraryExAFn = HMODULE (WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFn = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);

LoadLibraryAFn g_load_library_a = ::LoadLibraryA;
LoadLibraryWFn g_load_library_w = ::LoadLibraryW;
LoadLibraryExAFn g_load_library_ex_a = ::LoadLibraryExA;
LoadLibraryExWFn g_load_library_ex_w = ::LoadLibraryExW;

bool g_loader_hooks_installed = false;

struct EmulatedSleepJob {
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore signal_semaphore = VK_NULL_HANDLE;
    uint64_t signal_value = 0;
};

constexpr std::size_t kSleepQueueCapacity = 64;
// Bounded zero-allocation MPMC ring. Producers never take a lock and the
// worker consumes directly from fixed storage.
FixedMpmcRing<EmulatedSleepJob, kSleepQueueCapacity> g_sleep_queue{};
std::atomic<bool> g_sleep_worker_waiting{false};
std::atomic<bool> g_sleep_worker_ready{false};
std::atomic<bool> g_sleep_worker_stop{false};
std::atomic<bool> g_translation_bypass{false};
std::mutex g_sleep_worker_init_mutex;
HANDLE g_sleep_event = nullptr;
HANDLE g_sleep_thread = nullptr;

bool g_loader_a_attached = false;
bool g_loader_w_attached = false;
bool g_loader_ex_a_attached = false;
bool g_loader_ex_w_attached = false;

bool g_vk_gipa_attached = false;
bool g_vk_gdpa_attached = false;
bool g_vk_create_device_attached = false;
bool g_vk_destroy_device_attached = false;
bool g_vk_enum_extensions_attached = false;
bool g_vk_get_device_queue_attached = false;
bool g_vk_get_device_queue2_attached = false;
bool g_vk_acquire_next_image_attached = false;
bool g_vk_acquire_next_image2_attached = false;
bool g_vk_create_swapchain_attached = false;
bool g_vk_queue_present_attached = false;
bool g_vk_destroy_swapchain_attached = false;


bool module_is_outside_system_directory(const char* module_name) {
    const HMODULE module = GetModuleHandleA(module_name);
    if (!module)
        return false;

    char module_path[MAX_PATH]{};
    char system_directory[MAX_PATH]{};
    const DWORD module_length = GetModuleFileNameA(module, module_path, MAX_PATH);
    const UINT system_length = GetSystemDirectoryA(system_directory, MAX_PATH);
    if (module_length == 0 || module_length >= MAX_PATH ||
        system_length == 0 || system_length >= MAX_PATH) {
        return false;
    }

    const std::size_t prefix_length = std::strlen(system_directory);
    if (_strnicmp(module_path, system_directory, prefix_length) != 0)
        return true;

    const char next = module_path[prefix_length];
    return next != '\\' && next != '/' && next != '\0';
}

enum class VulkanStackLayer : std::uint8_t {
    None = 0,
    OptiScalerOrGameProxy = 1u << 0,
    Vkd3dProton = 1u << 1,
    Dxvk = 1u << 2,
};

using VulkanStackLayers = std::uint8_t;

constexpr VulkanStackLayers stack_layer_bit(VulkanStackLayer layer) noexcept {
    return static_cast<VulkanStackLayers>(layer);
}

constexpr bool stack_has_layer(VulkanStackLayers layers, VulkanStackLayer layer) noexcept {
    return (layers & stack_layer_bit(layer)) != 0;
}

constexpr bool stack_has_translation_layer(VulkanStackLayers layers) noexcept {
    return stack_has_layer(layers, VulkanStackLayer::Vkd3dProton) ||
           stack_has_layer(layers, VulkanStackLayer::Dxvk);
}

bool running_under_wine() {
    const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

VulkanStackLayers detect_existing_vulkan_stack(HMODULE vulkan_module) {
    // Transport classification must work even before vulkan-1.dll is loaded.
    // This is critical for delayed-loader startup: D3D11/D3D12 translation
    // layers may already be present while the Vulkan loader itself is not.
    (void)vulkan_module;
    VulkanStackLayers layers = stack_layer_bit(VulkanStackLayer::None);

    // Record the game-facing proxy layer without returning early. OptiScaler
    // can sit on top of vkd3d-proton or DXVK, and that lower translation layer
    // matters to low-level backend selection even when the proxy is detected
    // first. The stack is therefore represented as composable layer bits.
    if (GetModuleHandleA("optiscaler.dll") ||
        module_is_outside_system_directory("d3d12.dll") ||
        module_is_outside_system_directory("d3d11.dll") ||
        module_is_outside_system_directory("dxgi.dll")) {
        layers |= stack_layer_bit(VulkanStackLayer::OptiScalerOrGameProxy);
    }

    // Never infer a D3D->Vulkan translation layer on native Windows merely
    // because vulkan-1.dll happens to be loaded. Under Wine, however, the
    // simultaneous presence of D3D12+Vulkan is the vkd3d-proton shape and
    // D3D11+Vulkan is the DXVK shape. Both can coexist with a game-local proxy.
    if (running_under_wine()) {
        if (GetModuleHandleA("d3d12.dll"))
            layers |= stack_layer_bit(VulkanStackLayer::Vkd3dProton);
        if (GetModuleHandleA("d3d11.dll"))
            layers |= stack_layer_bit(VulkanStackLayer::Dxvk);
    }

    return layers;
}

const char* vulkan_stack_name(VulkanStackLayers layers) noexcept {
    const bool proxy = stack_has_layer(layers, VulkanStackLayer::OptiScalerOrGameProxy);
    const bool vkd3d = stack_has_layer(layers, VulkanStackLayer::Vkd3dProton);
    const bool dxvk = stack_has_layer(layers, VulkanStackLayer::Dxvk);

    if (proxy && vkd3d && dxvk)
        return "OptiScaler/game-local graphics proxy over Wine D3D12/D3D11+Vulkan stack (vkd3d-proton/DXVK likely)";
    if (proxy && vkd3d)
        return "OptiScaler/game-local graphics proxy over Wine D3D12+Vulkan stack (vkd3d-proton likely)";
    if (proxy && dxvk)
        return "OptiScaler/game-local graphics proxy over Wine D3D11+Vulkan stack (DXVK likely)";
    if (vkd3d && dxvk)
        return "Wine D3D12/D3D11+Vulkan stack (vkd3d-proton/DXVK likely)";
    if (proxy)
        return "OptiScaler/game-local graphics proxy";
    if (vkd3d)
        return "Wine D3D12+Vulkan stack (vkd3d-proton likely)";
    if (dxvk)
        return "Wine D3D11+Vulkan stack (DXVK likely)";
    return "none";
}

bool has_enabled_extension(const VkDeviceCreateInfo& create_info, const char* extension_name) {
    if (!create_info.ppEnabledExtensionNames)
        return false;

    for (uint32_t i = 0; i < create_info.enabledExtensionCount; ++i) {
        if (std::strcmp(create_info.ppEnabledExtensionNames[i], extension_name) == 0)
            return true;
    }
    return false;
}

bool physical_device_supports_extension(
    VkPhysicalDevice physical_device,
    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions,
    const char* extension_name) {

    if (!enumerate_extensions)
        return false;

    uint32_t count = 0;
    if (enumerate_extensions(physical_device, nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
        return false;

    std::vector<VkExtensionProperties> extensions(count);
    if (enumerate_extensions(physical_device, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;

    return std::any_of(extensions.begin(), extensions.end(), [extension_name](const auto& extension) {
        return std::strcmp(extension.extensionName, extension_name) == 0;
    });
}

const VkBaseInStructure* find_pnext_structure(const void* p_next, VkStructureType type) {
    auto* current = static_cast<const VkBaseInStructure*>(p_next);
    while (current) {
        if (current->sType == type)
            return current;
        current = current->pNext;
    }
    return nullptr;
}

struct PresentMetadata {
    std::uint64_t present_id = 0;
    bool uses_present_id2 = false;
    bool timing_requested = false;
};

struct PresentMetadataView {
    const VkPresentIdKHR* ids = nullptr;
#ifdef VK_KHR_present_id2
    const VkPresentId2KHR* ids2 = nullptr;
#endif
#ifdef VK_EXT_present_timing
    const VkPresentTimingsInfoEXT* timings = nullptr;
#endif
};

PresentMetadataView parse_present_metadata(const VkPresentInfoKHR& present_info) noexcept {
    PresentMetadataView view{};
    const auto* current = static_cast<const VkBaseInStructure*>(present_info.pNext);
    while (current) {
        if (current->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) {
            view.ids = reinterpret_cast<const VkPresentIdKHR*>(current);
        }
#ifdef VK_KHR_present_id2
        else if (current->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) {
            view.ids2 = reinterpret_cast<const VkPresentId2KHR*>(current);
        }
#endif
#ifdef VK_EXT_present_timing
        else if (current->sType == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) {
            view.timings = reinterpret_cast<const VkPresentTimingsInfoEXT*>(current);
        }
#endif
        current = current->pNext;
    }
    return view;
}

PresentMetadata present_metadata_at(
    const VkPresentInfoKHR& present_info,
    const PresentMetadataView& view,
    std::uint32_t index) noexcept {
    PresentMetadata metadata{};
#ifdef VK_KHR_present_id2
    if (view.ids2 && view.ids2->swapchainCount == present_info.swapchainCount &&
        view.ids2->pPresentIds && index < view.ids2->swapchainCount) {
        metadata.present_id = view.ids2->pPresentIds[index];
        metadata.uses_present_id2 = true;
    } else
#endif
    if (view.ids && view.ids->swapchainCount == present_info.swapchainCount &&
        view.ids->pPresentIds && index < view.ids->swapchainCount) {
        metadata.present_id = view.ids->pPresentIds[index];
    }
#ifdef VK_EXT_present_timing
    if (view.timings && view.timings->swapchainCount == present_info.swapchainCount &&
        view.timings->pTimingInfos && index < view.timings->swapchainCount) {
        metadata.timing_requested = view.timings->pTimingInfos[index].presentStageQueries != 0;
    }
#endif
    return metadata;
}

bool uses_multi_device_group(const VkDeviceCreateInfo& create_info) {
    const auto* base = find_pnext_structure(
        create_info.pNext,
        VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO);

    if (!base)
        return false;

    const auto* group = reinterpret_cast<const VkDeviceGroupDeviceCreateInfo*>(base);
    return group->physicalDeviceCount > 1;
}

bool nv_low_latency2_emulation_prerequisites_supported(
    VkPhysicalDevice physical_device,
    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions,
    PFN_vkGetPhysicalDeviceProperties get_properties) {
    if (!get_properties)
        return false;

    VkPhysicalDeviceProperties properties{};
    get_properties(physical_device, &properties);

    const bool timeline_supported =
        properties.apiVersion >= VK_API_VERSION_1_2 ||
        physical_device_supports_extension(
            physical_device,
            enumerate_extensions,
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);

    const bool present_id_supported =
        physical_device_supports_extension(
            physical_device,
            enumerate_extensions,
            VK_KHR_PRESENT_ID_EXTENSION_NAME) ||
        physical_device_supports_extension(
            physical_device,
            enumerate_extensions,
            VK_KHR_PRESENT_ID_2_EXTENSION_NAME);

    return timeline_supported && present_id_supported;
}

bool nv_low_latency2_emulation_prerequisites_enabled(
    VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo& create_info,
    PFN_vkGetPhysicalDeviceProperties get_properties) {
    if (!get_properties)
        return false;

    VkPhysicalDeviceProperties properties{};
    get_properties(physical_device, &properties);

    const bool timeline_enabled =
        properties.apiVersion >= VK_API_VERSION_1_2 ||
        has_enabled_extension(create_info, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);

    // present_id / present_id2 remain device extensions and therefore must be
    // enabled by the application when it enables the emulated NV extension.
    const bool present_id_enabled =
        has_enabled_extension(create_info, VK_KHR_PRESENT_ID_EXTENSION_NAME) ||
        has_enabled_extension(create_info, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);

    return timeline_enabled && present_id_enabled;
}

bool should_advertise_nv_low_latency2(
    const policy::RuntimePolicySnapshot& snapshot,
    bool native_support,
    bool emulation_prerequisites_supported) noexcept {
    switch (snapshot.vulkan.expose_nv_low_latency2) {
        case policy::AutoBool::Disabled:
            return false;
        case policy::AutoBool::Enabled:
            return native_support || emulation_prerequisites_supported;
        case policy::AutoBool::Auto:
            if (native_support)
                return true;
            return emulation_prerequisites_supported &&
                   snapshot.vulkan.spoofing != policy::VulkanSpoofing::Off;
    }
    return native_support;
}

bool should_use_native_nv_low_latency2(
    const policy::RuntimePolicySnapshot& snapshot,
    bool native_support) noexcept {
    if (!native_support || !snapshot.vulkan.prefer_native_extensions)
        return false;

    return snapshot.output.vulkan == policy::Backend::Auto ||
           snapshot.output.vulkan == policy::Backend::NativeReflex;
}

std::optional<MarkerType> marker_from_vk(VkLatencyMarkerNV marker) noexcept {
    switch (marker) {
        case VK_LATENCY_MARKER_SIMULATION_START_NV: return MarkerType::SIMULATION_START;
        case VK_LATENCY_MARKER_SIMULATION_END_NV: return MarkerType::SIMULATION_END;
        case VK_LATENCY_MARKER_RENDERSUBMIT_START_NV: return MarkerType::RENDERSUBMIT_START;
        case VK_LATENCY_MARKER_RENDERSUBMIT_END_NV: return MarkerType::RENDERSUBMIT_END;
        case VK_LATENCY_MARKER_PRESENT_START_NV: return MarkerType::PRESENT_START;
        case VK_LATENCY_MARKER_PRESENT_END_NV: return MarkerType::PRESENT_END;
        case VK_LATENCY_MARKER_INPUT_SAMPLE_NV: return MarkerType::INPUT_SAMPLE;
        case VK_LATENCY_MARKER_TRIGGER_FLASH_NV: return MarkerType::TRIGGER_FLASH;
        case VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV: return MarkerType::OUT_OF_BAND_RENDERSUBMIT_START;
        case VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_END_NV: return MarkerType::OUT_OF_BAND_RENDERSUBMIT_END;
        case VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV: return MarkerType::OUT_OF_BAND_PRESENT_START;
        case VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV: return MarkerType::OUT_OF_BAND_PRESENT_END;
        case VK_LATENCY_MARKER_MAX_ENUM_NV: break;
    }
    return std::nullopt;
}

bool is_nv_low_latency2_name(const char* name) noexcept {
    if (!name) return false;
    return std::strcmp(name, "vkSetLatencySleepModeNV") == 0 ||
           std::strcmp(name, "vkLatencySleepNV") == 0 ||
           std::strcmp(name, "vkSetLatencyMarkerNV") == 0 ||
           std::strcmp(name, "vkGetLatencyTimingsNV") == 0 ||
           std::strcmp(name, "vkQueueNotifyOutOfBandNV") == 0;
}

PFN_vkVoidFunction nv_low_latency2_hook_by_name(const char* name) noexcept {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkSetLatencySleepModeNV") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(VulkanHooks::hkvkSetLatencySleepModeNV);
    if (std::strcmp(name, "vkLatencySleepNV") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(VulkanHooks::hkvkLatencySleepNV);
    if (std::strcmp(name, "vkSetLatencyMarkerNV") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(VulkanHooks::hkvkSetLatencyMarkerNV);
    if (std::strcmp(name, "vkGetLatencyTimingsNV") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(VulkanHooks::hkvkGetLatencyTimingsNV);
    if (std::strcmp(name, "vkQueueNotifyOutOfBandNV") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(VulkanHooks::hkvkQueueNotifyOutOfBandNV);
    return nullptr;
}

bool try_dequeue_emulated_sleep(EmulatedSleepJob* out_job) noexcept {
    return g_sleep_queue.try_dequeue(out_job);
}

void process_emulated_sleep(const EmulatedSleepJob& job) {
    if (auto* low_latency = LowLatencyCtx::get()) {
        low_latency->VulkanFrontendDriveSleep(
            policy::InputFrontend::VkNvLowLatency2,
            job.device);
    }

    VulkanDeviceRegistry::signal_semaphore(
        job.device,
        job.signal_semaphore,
        job.signal_value);
}

DWORD WINAPI emulated_sleep_worker(LPVOID) {
    for (;;) {
        EmulatedSleepJob job{};
        while (try_dequeue_emulated_sleep(&job)) {
            process_emulated_sleep(job);
        }

        if (g_sleep_worker_stop.load(std::memory_order_acquire))
            break;

        // Arm the sleep state before the empty recheck. A producer racing this
        // transition either observes `waiting` and signals the event, or its
        // already-published cell is found by the recheck below. No lost wakeup.
        g_sleep_worker_waiting.store(true, std::memory_order_release);
        if (try_dequeue_emulated_sleep(&job)) {
            g_sleep_worker_waiting.store(false, std::memory_order_release);
            process_emulated_sleep(job);
            continue;
        }

        if (g_sleep_worker_stop.load(std::memory_order_acquire)) {
            g_sleep_worker_waiting.store(false, std::memory_order_release);
            break;
        }

        if (WaitForSingleObject(g_sleep_event, INFINITE) != WAIT_OBJECT_0) {
            g_sleep_worker_waiting.store(false, std::memory_order_release);
            break;
        }
        g_sleep_worker_waiting.store(false, std::memory_order_release);
    }

    // Stop is fail-safe: finish jobs already committed before teardown.
    EmulatedSleepJob job{};
    while (try_dequeue_emulated_sleep(&job)) {
        process_emulated_sleep(job);
    }
    return 0;
}

bool ensure_sleep_worker() {
    if (g_sleep_worker_ready.load(std::memory_order_acquire))
        return true;

    std::scoped_lock lock(g_sleep_worker_init_mutex);
    if (g_sleep_worker_ready.load(std::memory_order_relaxed))
        return true;

    g_sleep_queue.reset();
    g_sleep_worker_waiting.store(false, std::memory_order_relaxed);

    g_sleep_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_sleep_event)
        return false;

    g_sleep_worker_stop.store(false, std::memory_order_release);
    g_sleep_thread = CreateThread(nullptr, 0, emulated_sleep_worker, nullptr, 0, nullptr);
    if (!g_sleep_thread) {
        CloseHandle(g_sleep_event);
        g_sleep_event = nullptr;
        return false;
    }

    g_sleep_worker_ready.store(true, std::memory_order_release);
    return true;
}

bool enqueue_emulated_sleep(VkDevice device, const VkLatencySleepInfoNV& sleep_info) {
    if (!ensure_sleep_worker())
        return false;

    const EmulatedSleepJob job{
        .device = device,
        .signal_semaphore = sleep_info.signalSemaphore,
        .signal_value = sleep_info.value,
    };
    if (!g_sleep_queue.try_enqueue(job))
        return false; // bounded queue full: caller signals fail-open

    // Avoid a locked RMW on the steady producer path. While the worker is
    // draining, waiting=false and producers perform only a read. CAS is paid
    // solely by the producer that actually disarms a sleeping worker.
    if (g_sleep_worker_waiting.load(std::memory_order_acquire)) {
        bool expected = true;
        if (g_sleep_worker_waiting.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel, std::memory_order_acquire)) {
            SetEvent(g_sleep_event);
        }
    }
    return true;
}

void stop_sleep_worker() {
    // Serialize teardown against a late first-use initialization attempt.
    std::scoped_lock init_lock(g_sleep_worker_init_mutex);
    if (!g_sleep_worker_ready.exchange(false, std::memory_order_acq_rel))
        return;

    g_sleep_worker_stop.store(true, std::memory_order_release);
    if (g_sleep_event)
        SetEvent(g_sleep_event);

    if (g_sleep_thread) {
        WaitForSingleObject(g_sleep_thread, INFINITE);
        CloseHandle(g_sleep_thread);
        g_sleep_thread = nullptr;
    }
    if (g_sleep_event) {
        CloseHandle(g_sleep_event);
        g_sleep_event = nullptr;
    }

    g_sleep_queue.reset();
    g_sleep_worker_waiting.store(false, std::memory_order_relaxed);
    g_sleep_worker_stop.store(false, std::memory_order_release);
}

void try_hook_loaded_vulkan() {
    if (g_translation_bypass.load(std::memory_order_acquire))
        return;
    if (HMODULE module = GetModuleHandleA("vulkan-1.dll"))
        VulkanHooks::initialize(module);
}

void try_hook_loaded_openxr() {
    if (HMODULE module = GetModuleHandleA("openxr_loader.dll"))
        OpenXRHooks::initialize(module);
}

void try_hook_late_modules() {
    try_hook_loaded_vulkan();
    try_hook_loaded_openxr();
}

HMODULE WINAPI hkLoadLibraryA(LPCSTR file_name) {
    HMODULE module = g_load_library_a ? g_load_library_a(file_name) : nullptr;
    try_hook_late_modules();
    return module;
}

HMODULE WINAPI hkLoadLibraryW(LPCWSTR file_name) {
    HMODULE module = g_load_library_w ? g_load_library_w(file_name) : nullptr;
    try_hook_late_modules();
    return module;
}

HMODULE WINAPI hkLoadLibraryExA(LPCSTR file_name, HANDLE file, DWORD flags) {
    HMODULE module = g_load_library_ex_a ? g_load_library_ex_a(file_name, file, flags) : nullptr;
    try_hook_late_modules();
    return module;
}

HMODULE WINAPI hkLoadLibraryExW(LPCWSTR file_name, HANDLE file, DWORD flags) {
    HMODULE module = g_load_library_ex_w ? g_load_library_ex_w(file_name, file, flags) : nullptr;
    try_hook_late_modules();
    return module;
}

void install_loader_hooks() {
    std::scoped_lock lock(g_hook_mutex);
    if (g_loader_hooks_installed)
        return;

    const LONG begin_result = DetourTransactionBegin();
    if (begin_result != NO_ERROR) {
        spdlog::error("DetourTransactionBegin failed for Vulkan loader watcher: {}", begin_result);
        return;
    }

    DetourUpdateThread(GetCurrentThread());

    std::array<PVOID, 4> seen_targets{};
    std::size_t seen_count = 0;

    const auto attach_unique = [&](PVOID* target_ptr, PVOID detour, bool& attached) {
        if (!target_ptr || !*target_ptr)
            return;

        const PVOID original_target = *target_ptr;
        for (std::size_t i = 0; i < seen_count; ++i) {
            if (seen_targets[i] == original_target)
                return;
        }

        const LONG result = DetourAttach(target_ptr, detour);
        if (result == NO_ERROR) {
            seen_targets[seen_count++] = original_target;
            attached = true;
        }
    };

    attach_unique(reinterpret_cast<PVOID*>(&g_load_library_a), reinterpret_cast<PVOID>(hkLoadLibraryA), g_loader_a_attached);
    attach_unique(reinterpret_cast<PVOID*>(&g_load_library_w), reinterpret_cast<PVOID>(hkLoadLibraryW), g_loader_w_attached);
    attach_unique(reinterpret_cast<PVOID*>(&g_load_library_ex_a), reinterpret_cast<PVOID>(hkLoadLibraryExA), g_loader_ex_a_attached);
    attach_unique(reinterpret_cast<PVOID*>(&g_load_library_ex_w), reinterpret_cast<PVOID>(hkLoadLibraryExW), g_loader_ex_w_attached);

    const LONG commit_result = DetourTransactionCommit();
    if (commit_result != NO_ERROR) {
        g_loader_a_attached = false;
        g_loader_w_attached = false;
        g_loader_ex_a_attached = false;
        g_loader_ex_w_attached = false;
        spdlog::error("DetourTransactionCommit failed for Vulkan loader watcher: {}", commit_result);
        return;
    }

    g_loader_hooks_installed = true;
    spdlog::info("Graphics/XR delayed-loader watcher installed");
}

VkResult enumerate_policy_extensions(
    VkPhysicalDevice physical_device,
    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions,
    PFN_vkGetPhysicalDeviceProperties get_properties,
    uint32_t* property_count,
    VkExtensionProperties* properties) {
    if (!enumerate_extensions || !property_count)
        return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t native_count = 0;
    VkResult result = enumerate_extensions(
        physical_device, nullptr, &native_count, nullptr);
    if (result != VK_SUCCESS)
        return result;

    std::vector<VkExtensionProperties> native_extensions(native_count);
    if (native_count > 0) {
        result = enumerate_extensions(
            physical_device, nullptr, &native_count, native_extensions.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            return result;
        native_extensions.resize(native_count);
    }

    const bool native_nv = std::any_of(native_extensions.begin(), native_extensions.end(), [](const auto& extension) {
        return std::strcmp(extension.extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0;
    });
    const bool emulation_prerequisites_supported =
        nv_low_latency2_emulation_prerequisites_supported(
            physical_device,
            enumerate_extensions,
            get_properties);
    const bool advertise_nv = should_advertise_nv_low_latency2(
        Config::get().snapshot(),
        native_nv,
        emulation_prerequisites_supported);

    std::vector<VkExtensionProperties> visible_extensions;
    visible_extensions.reserve(native_extensions.size() + 1);
    for (const auto& extension : native_extensions) {
        if (std::strcmp(extension.extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) != 0)
            visible_extensions.push_back(extension);
    }

    if (advertise_nv) {
        if (native_nv) {
            const auto it = std::find_if(native_extensions.begin(), native_extensions.end(), [](const auto& extension) {
                return std::strcmp(extension.extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0;
            });
            if (it != native_extensions.end())
                visible_extensions.push_back(*it);
        } else {
            VkExtensionProperties emulated{};
            std::strncpy(
                emulated.extensionName,
                VK_NV_LOW_LATENCY_2_EXTENSION_NAME,
                VK_MAX_EXTENSION_NAME_SIZE - 1);
            emulated.specVersion = VK_NV_LOW_LATENCY_2_SPEC_VERSION;
            visible_extensions.push_back(emulated);
        }
    }

    if (!properties) {
        *property_count = static_cast<uint32_t>(visible_extensions.size());
        return VK_SUCCESS;
    }

    const uint32_t capacity = *property_count;
    const uint32_t written = std::min<uint32_t>(capacity, static_cast<uint32_t>(visible_extensions.size()));
    if (written > 0)
        std::copy_n(visible_extensions.data(), written, properties);
    *property_count = written;

    return written < visible_extensions.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
} // namespace

void VulkanHooks::initialize(HMODULE vulkan_module) {
    if (!vulkan_module)
        vulkan_module = GetModuleHandleA("vulkan-1.dll");

    const auto& snapshot = Config::get().snapshot();
    // R4.1: the existing delayed-loader watcher is shared with XRFlex, but it
    // is installed only when a graphics/XR loader is actually still missing.
    // Processes with both loaders already present pay no new LoadLibrary detour.
    const bool openxr_pending =
        snapshot.openxr.enabled && GetModuleHandleA("openxr_loader.dll") == nullptr;
    if (!vulkan_module || openxr_pending)
        install_loader_hooks();
    const bool force_nv_low_latency2 =
        snapshot.vulkan.expose_nv_low_latency2 == policy::AutoBool::Enabled;
    const bool force_amd_anti_lag =
        snapshot.vulkan.expose_amd_anti_lag == policy::AutoBool::Enabled;
    const VulkanStackLayers stack_layers = detect_existing_vulkan_stack(vulkan_module);
    const bool translation_stack = stack_has_translation_layer(stack_layers);
    const bool vulkanflex_requested = snapshot.vulkanflex.enabled &&
        (!snapshot.vulkanflex.only_native_vulkan || !translation_stack) &&
        (snapshot.output.vulkan == policy::Backend::VulkanFlex ||
         (snapshot.output.vulkan == policy::Backend::Auto &&
          snapshot.backend_order.vulkan.contains(policy::Backend::VulkanFlex)));

    // Transport ownership is stronger than backend preference. With
    // only_native_vulkan enabled, a Wine D3D->Vulkan translation layer owns
    // the Vulkan chain completely. Do not install core/WSI detours at all: a
    // late on_acquire() guard is insufficient because hook stacking itself can
    // conflict with DXVK/vkd3d-proton/OptiScaler during VkDevice creation.
    // Explicit Vulkan extension forcing remains the opt-in escape hatch for
    // targeted compatibility work.
    if (translation_stack && snapshot.vulkanflex.only_native_vulkan &&
        !force_nv_low_latency2 && !force_amd_anti_lag) {
        transport_truth::TransportTruth::note_translation_bypass_suspected();
        g_translation_bypass.store(true, std::memory_order_release);
        spdlog::warn(
            "Vulkan transport bypass: detected {}; VulkanFlex native_only owns no Vulkan hooks on translation stacks. "
            "Skipping Vulkan core/WSI detours; D3D/Reflex/XeLL/Anti-Lag2 frontends remain available.",
            vulkan_stack_name(stack_layers));
        return;
    }

    if (vulkan_module &&
        !force_nv_low_latency2 &&
        !force_amd_anti_lag &&
        !vulkanflex_requested &&
        stack_layers != stack_layer_bit(VulkanStackLayer::None)) {
        spdlog::warn(
            "Vulkan auto coexistence: detected {}; skipping Vulkan core detours because the existing "
            "graphics stack already owns the Vulkan chain. Reflex/XeLL/Anti-Lag2 frontends remain available. "
            "Set [vulkan] expose_nv_low_latency2=enabled to force Vulkan interception.",
            vulkan_stack_name(stack_layers));
        return;
    }

    if (vulkan_module) {
        hook_vulkan(vulkan_module);
        return;
    }

    install_loader_hooks();
}

void VulkanHooks::shutdown() {
    stop_sleep_worker();
    std::scoped_lock lock(g_hook_mutex);

    if (!g_vulkan_hooks_installed && !g_loader_hooks_installed) {
        g_translation_bypass.store(false, std::memory_order_release);
        transport_truth::TransportTruth::reset();
        return;
    }

    if (DetourTransactionBegin() != NO_ERROR)
        return;

    DetourUpdateThread(GetCurrentThread());

    if (g_vk_gipa_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkGetInstanceProcAddr), reinterpret_cast<PVOID>(hkvkGetInstanceProcAddr));
    if (g_vk_gdpa_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkGetDeviceProcAddr), reinterpret_cast<PVOID>(hkvkGetDeviceProcAddr));
    if (g_vk_create_device_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkCreateDevice), reinterpret_cast<PVOID>(hkvkCreateDevice));
    if (g_vk_destroy_device_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkDestroyDevice), reinterpret_cast<PVOID>(hkvkDestroyDevice));
    if (g_vk_enum_extensions_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkEnumerateDeviceExtensionProperties), reinterpret_cast<PVOID>(hkvkEnumerateDeviceExtensionProperties));
    if (g_vk_get_device_queue_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkGetDeviceQueue), reinterpret_cast<PVOID>(hkvkGetDeviceQueue));
    if (g_vk_get_device_queue2_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkGetDeviceQueue2), reinterpret_cast<PVOID>(hkvkGetDeviceQueue2));
    if (g_vk_acquire_next_image_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkAcquireNextImageKHR), reinterpret_cast<PVOID>(hkvkAcquireNextImageKHR));
    if (g_vk_acquire_next_image2_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkAcquireNextImage2KHR), reinterpret_cast<PVOID>(hkvkAcquireNextImage2KHR));
    if (g_vk_create_swapchain_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkCreateSwapchainKHR), reinterpret_cast<PVOID>(hkvkCreateSwapchainKHR));
    if (g_vk_queue_present_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkQueuePresentKHR), reinterpret_cast<PVOID>(hkvkQueuePresentKHR));
    if (g_vk_destroy_swapchain_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&o_vkDestroySwapchainKHR), reinterpret_cast<PVOID>(hkvkDestroySwapchainKHR));

    if (g_loader_a_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&g_load_library_a), reinterpret_cast<PVOID>(hkLoadLibraryA));
    if (g_loader_w_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&g_load_library_w), reinterpret_cast<PVOID>(hkLoadLibraryW));
    if (g_loader_ex_a_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&g_load_library_ex_a), reinterpret_cast<PVOID>(hkLoadLibraryExA));
    if (g_loader_ex_w_attached)
        DetourDetach(reinterpret_cast<PVOID*>(&g_load_library_ex_w), reinterpret_cast<PVOID>(hkLoadLibraryExW));

    if (DetourTransactionCommit() == NO_ERROR) {
        g_vulkan_hooks_installed = false;
        g_loader_hooks_installed = false;
        g_hooked_vulkan_module = nullptr;
        g_translation_bypass.store(false, std::memory_order_release);
        transport_truth::TransportTruth::reset();
        g_vk_gipa_attached = false;
        g_vk_gdpa_attached = false;
        g_vk_create_device_attached = false;
        g_vk_destroy_device_attached = false;
        g_vk_enum_extensions_attached = false;
        g_vk_get_device_queue_attached = false;
        g_vk_get_device_queue2_attached = false;
        g_vk_acquire_next_image_attached = false;
        g_vk_acquire_next_image2_attached = false;
        g_vk_create_swapchain_attached = false;
        g_vk_queue_present_attached = false;
        g_vk_destroy_swapchain_attached = false;
        g_loader_a_attached = false;
        g_loader_w_attached = false;
        g_loader_ex_a_attached = false;
        g_loader_ex_w_attached = false;
    }
}

VkResult VKAPI_CALL VulkanHooks::hkvkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device,
    const char* layer_name,
    uint32_t* property_count,
    VkExtensionProperties* properties) {
    if (!o_vkEnumerateDeviceExtensionProperties)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Do not rewrite extension lists reported by explicit layers. The Vulkan
    // loader's aggregate physical-device extension list is the only list that
    // is used for our driver-level compatibility exposure.
    if (layer_name)
        return o_vkEnumerateDeviceExtensionProperties(physical_device, layer_name, property_count, properties);

    return enumerate_policy_extensions(
        physical_device,
        o_vkEnumerateDeviceExtensionProperties,
        o_vkGetPhysicalDeviceProperties,
        property_count,
        properties);
}

VkResult VKAPI_CALL VulkanHooks::hkvkCreateDevice(
    VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkDevice* device) {

    if (!o_vkCreateDevice || !create_info || !device)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkDeviceCreateInfo modified_create_info = *create_info;
    std::vector<const char*> extensions;
    extensions.reserve(create_info->enabledExtensionCount + 1);
    if (create_info->enabledExtensionCount > 0 && create_info->ppEnabledExtensionNames) {
        extensions.insert(
            extensions.end(),
            create_info->ppEnabledExtensionNames,
            create_info->ppEnabledExtensionNames + create_info->enabledExtensionCount);
    }

    const auto& snapshot = Config::get().snapshot();
    const bool nv_requested = has_enabled_extension(*create_info, VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    const bool nv_native_supported = physical_device_supports_extension(
        physical_device,
        o_vkEnumerateDeviceExtensionProperties,
        VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    const bool nv_emulation_prerequisites_supported =
        nv_low_latency2_emulation_prerequisites_supported(
            physical_device,
            o_vkEnumerateDeviceExtensionProperties,
            o_vkGetPhysicalDeviceProperties);
    const bool nv_advertised = should_advertise_nv_low_latency2(
        snapshot,
        nv_native_supported,
        nv_emulation_prerequisites_supported);

    if (nv_requested && uses_multi_device_group(*create_info)) {
        spdlog::warn("VK_NV_low_latency2 rejected: device groups are not supported");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    if (nv_requested && !nv_advertised) {
        spdlog::warn("VK_NV_low_latency2 requested but native support/emulation prerequisites are unavailable");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    const bool nv_use_native = nv_requested &&
        should_use_native_nv_low_latency2(snapshot, nv_native_supported);
    const bool nv_emulated = nv_requested && nv_advertised && !nv_use_native;

    if (nv_emulated && !nv_low_latency2_emulation_prerequisites_enabled(
            physical_device,
            *create_info,
            o_vkGetPhysicalDeviceProperties)) {
        spdlog::warn(
            "VK_NV_low_latency2 emulation requires Vulkan 1.2/VK_KHR_timeline_semaphore and "
            "VK_KHR_present_id or VK_KHR_present_id2 to be enabled");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    bool extension_list_modified = false;
    if (nv_emulated) {
        const auto old_size = extensions.size();
        extensions.erase(
            std::remove_if(extensions.begin(), extensions.end(), [](const char* extension) {
                return extension && std::strcmp(extension, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0;
            }),
            extensions.end());
        extension_list_modified = extensions.size() != old_size;
    }

    bool anti_lag_supported = false;
    bool anti_lag_enabled = false;

    VkPhysicalDeviceAntiLagFeaturesAMD anti_lag_enable{};
    anti_lag_enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD;
    anti_lag_enable.antiLag = VK_TRUE;

    const bool anti_lag_extension_supported = physical_device_supports_extension(
        physical_device,
        o_vkEnumerateDeviceExtensionProperties,
        VK_AMD_ANTI_LAG_EXTENSION_NAME);

    if (anti_lag_extension_supported && o_vkGetPhysicalDeviceFeatures2 && !uses_multi_device_group(*create_info)) {
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        VkPhysicalDeviceAntiLagFeaturesAMD anti_lag_query{};
        anti_lag_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD;
        features2.pNext = &anti_lag_query;

        o_vkGetPhysicalDeviceFeatures2(physical_device, &features2);
        anti_lag_supported = anti_lag_query.antiLag == VK_TRUE;
    }

    if (anti_lag_supported) {
        const bool already_enabled = has_enabled_extension(*create_info, VK_AMD_ANTI_LAG_EXTENSION_NAME);
        if (!already_enabled) {
            extensions.push_back(VK_AMD_ANTI_LAG_EXTENSION_NAME);
            extension_list_modified = true;
        }

        const auto* existing_anti_lag = find_pnext_structure(
            create_info->pNext,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD);

        if (existing_anti_lag) {
            const auto* feature = reinterpret_cast<const VkPhysicalDeviceAntiLagFeaturesAMD*>(existing_anti_lag);
            anti_lag_enabled = feature->antiLag == VK_TRUE;
        } else {
            anti_lag_enable.pNext = const_cast<void*>(modified_create_info.pNext);
            modified_create_info.pNext = &anti_lag_enable;
            anti_lag_enabled = true;
        }
    } else if (anti_lag_extension_supported && uses_multi_device_group(*create_info)) {
        spdlog::info("Vulkan Anti-Lag skipped: multi-device VkDeviceGroup is in use");
    }

    if (extension_list_modified) {
        modified_create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        modified_create_info.ppEnabledExtensionNames = extensions.data();
    }

    // R3.9 capability discovery is passive and transport-neutral. Record
    // physical-device support plus the exact application-enabled state before
    // vkCreateDevice. The capability engine never mutates extension lists or
    // the application's feature chain.
    VulkanCapabilitySnapshot capability_snapshot{};
    const VulkanCapabilityProbeDispatch capability_dispatch{
        o_vkGetPhysicalDeviceProperties,
        o_vkGetPhysicalDeviceFeatures2,
        o_vkEnumerateDeviceExtensionProperties};
    if (probe_vulkan_capability_support(physical_device, capability_dispatch, &capability_snapshot))
        observe_vulkan_device_create_info(*create_info, &capability_snapshot);

    // R3.6 present precision is passive: record only extension/features that
    // the application itself enabled. No extension list or pNext mutation is
    // performed here for present precision.
    const bool precision_allowed = snapshot.vulkanflex.present_precision != policy::AutoBool::Disabled;
    const auto* present_id_node = find_pnext_structure(
        create_info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
    const auto* present_wait_node = find_pnext_structure(
        create_info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
#ifdef VK_KHR_present_id2
    const auto* present_id2_node = find_pnext_structure(
        create_info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR);
#else
    const VkBaseInStructure* present_id2_node = nullptr;
#endif
#ifdef VK_KHR_present_wait2
    const auto* present_wait2_node = find_pnext_structure(
        create_info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR);
#else
    const VkBaseInStructure* present_wait2_node = nullptr;
#endif
#ifdef VK_EXT_present_timing
    const auto* present_timing_node = find_pnext_structure(
        create_info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT);
#else
    const VkBaseInStructure* present_timing_node = nullptr;
#endif

    const bool present_id_enabled = precision_allowed &&
        has_enabled_extension(*create_info, VK_KHR_PRESENT_ID_EXTENSION_NAME) && present_id_node &&
        reinterpret_cast<const VkPhysicalDevicePresentIdFeaturesKHR*>(present_id_node)->presentId == VK_TRUE;
#ifdef VK_KHR_present_id2
    const bool present_id2_enabled = precision_allowed &&
        has_enabled_extension(*create_info, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) && present_id2_node &&
        reinterpret_cast<const VkPhysicalDevicePresentId2FeaturesKHR*>(present_id2_node)->presentId2 == VK_TRUE;
#else
    const bool present_id2_enabled = false;
#endif
    const bool present_wait_enabled = precision_allowed && present_id_enabled &&
        has_enabled_extension(*create_info, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) && present_wait_node &&
        reinterpret_cast<const VkPhysicalDevicePresentWaitFeaturesKHR*>(present_wait_node)->presentWait == VK_TRUE;
#ifdef VK_KHR_present_wait2
    const bool present_wait2_enabled = precision_allowed && present_id2_enabled &&
        has_enabled_extension(*create_info, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME) && present_wait2_node &&
        reinterpret_cast<const VkPhysicalDevicePresentWait2FeaturesKHR*>(present_wait2_node)->presentWait2 == VK_TRUE;
#else
    const bool present_wait2_enabled = false;
#endif
#ifdef VK_EXT_present_timing
    const bool present_timing_enabled = precision_allowed && present_id2_enabled &&
        has_enabled_extension(*create_info, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) && present_timing_node &&
        reinterpret_cast<const VkPhysicalDevicePresentTimingFeaturesEXT*>(present_timing_node)->presentTiming == VK_TRUE;
#else
    const bool present_timing_enabled = false;
#endif

    const VkResult result = o_vkCreateDevice(physical_device, &modified_create_info, allocator, device);
    if (result != VK_SUCCESS)
        return result;

    VulkanDeviceState state{};
    state.device = *device;
    state.amd_anti_lag_enabled = anti_lag_enabled;
    state.nv_low_latency2_enabled = nv_requested && nv_advertised;
    state.nv_low_latency2_native = nv_use_native;
    state.present_id_enabled = present_id_enabled;
    state.present_id2_enabled = present_id2_enabled;
    state.present_wait_enabled = present_wait_enabled;
    state.present_wait2_enabled = present_wait2_enabled;
    state.present_timing_enabled = present_timing_enabled;
    state.capabilities = capability_snapshot;

    if (o_vkGetDeviceProcAddr) {
        state.create_semaphore = reinterpret_cast<PFN_vkCreateSemaphore>(
            o_vkGetDeviceProcAddr(*device, "vkCreateSemaphore"));
        state.destroy_semaphore = reinterpret_cast<PFN_vkDestroySemaphore>(
            o_vkGetDeviceProcAddr(*device, "vkDestroySemaphore"));
        state.signal_semaphore = reinterpret_cast<PFN_vkSignalSemaphore>(
            o_vkGetDeviceProcAddr(*device, "vkSignalSemaphore"));
        if (!state.signal_semaphore) {
            state.signal_semaphore = reinterpret_cast<PFN_vkSignalSemaphore>(
                o_vkGetDeviceProcAddr(*device, "vkSignalSemaphoreKHR"));
        }
        state.get_device_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(
            o_vkGetDeviceProcAddr(*device, "vkGetDeviceQueue"));
        state.get_device_queue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
            o_vkGetDeviceProcAddr(*device, "vkGetDeviceQueue2"));
        state.acquire_next_image = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
            o_vkGetDeviceProcAddr(*device, "vkAcquireNextImageKHR"));
        state.acquire_next_image2 = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(
            o_vkGetDeviceProcAddr(*device, "vkAcquireNextImage2KHR"));
        state.create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
            o_vkGetDeviceProcAddr(*device, "vkCreateSwapchainKHR"));
        state.create_graphics_pipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
            o_vkGetDeviceProcAddr(*device, "vkCreateGraphicsPipelines"));
        state.create_compute_pipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(
            o_vkGetDeviceProcAddr(*device, "vkCreateComputePipelines"));
        state.queue_present = reinterpret_cast<PFN_vkQueuePresentKHR>(
            o_vkGetDeviceProcAddr(*device, "vkQueuePresentKHR"));
        state.destroy_swapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
            o_vkGetDeviceProcAddr(*device, "vkDestroySwapchainKHR"));

        if (state.present_wait_enabled) {
            state.wait_for_present = reinterpret_cast<PFN_vkWaitForPresentKHR>(
                o_vkGetDeviceProcAddr(*device, "vkWaitForPresentKHR"));
            state.present_wait_enabled = state.wait_for_present != nullptr;
        }
#ifdef VK_KHR_present_wait2
        if (state.present_wait2_enabled) {
            state.wait_for_present2 = reinterpret_cast<PFN_vkWaitForPresent2KHR>(
                o_vkGetDeviceProcAddr(*device, "vkWaitForPresent2KHR"));
            state.present_wait2_enabled = state.wait_for_present2 != nullptr;
        }
#endif
#ifdef VK_EXT_present_timing
        if (state.present_timing_enabled) {
            state.get_past_presentation_timing = reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(
                o_vkGetDeviceProcAddr(*device, "vkGetPastPresentationTimingEXT"));
            state.present_timing_enabled = state.get_past_presentation_timing != nullptr;
        }
#endif

        if (anti_lag_enabled) {
            state.anti_lag_update = reinterpret_cast<PFN_vkAntiLagUpdateAMD>(
                o_vkGetDeviceProcAddr(*device, "vkAntiLagUpdateAMD"));
            state.amd_anti_lag_enabled = state.anti_lag_update != nullptr;
        }

        if (nv_use_native) {
            state.nv_set_latency_sleep_mode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(
                o_vkGetDeviceProcAddr(*device, "vkSetLatencySleepModeNV"));
            state.nv_latency_sleep = reinterpret_cast<PFN_vkLatencySleepNV>(
                o_vkGetDeviceProcAddr(*device, "vkLatencySleepNV"));
            state.nv_set_latency_marker = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(
                o_vkGetDeviceProcAddr(*device, "vkSetLatencyMarkerNV"));
            state.nv_get_latency_timings = reinterpret_cast<PFN_vkGetLatencyTimingsNV>(
                o_vkGetDeviceProcAddr(*device, "vkGetLatencyTimingsNV"));
            state.nv_queue_notify_out_of_band = reinterpret_cast<PFN_vkQueueNotifyOutOfBandNV>(
                o_vkGetDeviceProcAddr(*device, "vkQueueNotifyOutOfBandNV"));

            state.nv_low_latency2_native = state.nv_set_latency_sleep_mode &&
                                           state.nv_latency_sleep &&
                                           state.nv_set_latency_marker;
        }
    }

    if (!VulkanDeviceRegistry::register_device(state)) {
        spdlog::warn("Failed to register Vulkan device state");
    } else {
        // Direct interception of the application's VkDevice creation is strong
        // native-Vulkan evidence. This is deliberately stronger than the
        // early Wine module heuristic used only to decide hook ownership.
        transport_truth::TransportTruth::confirm_native_vulkan_device();
        spdlog::info("Transport truth confirmed: native-vulkan via intercepted VkDevice");
    }

    if (state.amd_anti_lag_enabled)
        spdlog::info("Vulkan backend capability: VK_AMD_anti_lag available");

    if (state.nv_low_latency2_enabled) {
        spdlog::info(
            "Vulkan input capability: VK_NV_low_latency2 {}",
            state.nv_low_latency2_native ? "native pass-through" : "emulated frontend");
    }

    if (precision_allowed) {
        spdlog::info(
            "VulkanFlex present precision device caps: present_id={}, present_id2={}, present_wait={}, present_wait2={}, present_timing={}",
            state.present_id_enabled, state.present_id2_enabled, state.present_wait_enabled,
            state.present_wait2_enabled, state.present_timing_enabled);
    }

    if (state.capabilities.api_version != 0) {
        const auto& caps = state.capabilities;
        spdlog::info(
            "Vulkan capability profile: api={}.{}, vk13={}, vk14={}, dynamic_rendering={}/{}, sync2={}/{}, timeline={}/{}, EDS1={}/{}, EDS2={}/{}, EDS2.logicOp={}/{}, EDS2.patchCP={}/{}, EDS3={}/{} bits={}/{}, descriptor_buffer={}/{}, GPL={}/{}, shader_module_id={}/{}, unified_layouts={}/{}, maintenance5={}/{}, maintenance6={}/{}, pipeline_robustness={}/{}",
            VK_API_VERSION_MAJOR(caps.api_version), VK_API_VERSION_MINOR(caps.api_version),
            caps.supports(VulkanCapability::Vulkan13), caps.supports(VulkanCapability::Vulkan14),
            caps.supports(VulkanCapability::DynamicRendering), caps.is_enabled(VulkanCapability::DynamicRendering),
            caps.supports(VulkanCapability::Synchronization2), caps.is_enabled(VulkanCapability::Synchronization2),
            caps.supports(VulkanCapability::TimelineSemaphore), caps.is_enabled(VulkanCapability::TimelineSemaphore),
            caps.supports(VulkanCapability::ExtendedDynamicState1), caps.is_enabled(VulkanCapability::ExtendedDynamicState1),
            caps.supports(VulkanCapability::ExtendedDynamicState2), caps.is_enabled(VulkanCapability::ExtendedDynamicState2),
            caps.supports(VulkanCapability::ExtendedDynamicState2LogicOp), caps.is_enabled(VulkanCapability::ExtendedDynamicState2LogicOp),
            caps.supports(VulkanCapability::ExtendedDynamicState2PatchControlPoints), caps.is_enabled(VulkanCapability::ExtendedDynamicState2PatchControlPoints),
            caps.supports(VulkanCapability::ExtendedDynamicState3), caps.is_enabled(VulkanCapability::ExtendedDynamicState3),
            vulkan_eds3_feature_count(caps.eds3_supported), vulkan_eds3_feature_count(caps.eds3_enabled),
            caps.supports(VulkanCapability::DescriptorBuffer), caps.is_enabled(VulkanCapability::DescriptorBuffer),
            caps.supports(VulkanCapability::GraphicsPipelineLibrary), caps.is_enabled(VulkanCapability::GraphicsPipelineLibrary),
            caps.supports(VulkanCapability::ShaderModuleIdentifier), caps.is_enabled(VulkanCapability::ShaderModuleIdentifier),
            caps.supports(VulkanCapability::UnifiedImageLayouts), caps.is_enabled(VulkanCapability::UnifiedImageLayouts),
            caps.supports(VulkanCapability::Maintenance5), caps.is_enabled(VulkanCapability::Maintenance5),
            caps.supports(VulkanCapability::Maintenance6), caps.is_enabled(VulkanCapability::Maintenance6),
            caps.supports(VulkanCapability::PipelineRobustness), caps.is_enabled(VulkanCapability::PipelineRobustness));
    }

    return result;
}

void VKAPI_CALL VulkanHooks::hkvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    VulkanDeviceRegistry::unregister_device(device);

    if (o_vkDestroyDevice)
        o_vkDestroyDevice(device, allocator);
}

void VKAPI_CALL VulkanHooks::hkvkGetDeviceQueue(
    VkDevice device,
    uint32_t queue_family_index,
    uint32_t queue_index,
    VkQueue* queue) {
    VulkanDeviceState state{};
    PFN_vkGetDeviceQueue get_queue = nullptr;
    if (VulkanDeviceRegistry::get_state(device, &state))
        get_queue = state.get_device_queue;
    if (!get_queue)
        get_queue = o_vkGetDeviceQueue;

    if (!get_queue || !queue)
        return;

    get_queue(device, queue_family_index, queue_index, queue);
    if (*queue != VK_NULL_HANDLE)
        VulkanDeviceRegistry::register_queue(device, *queue);
}

void VKAPI_CALL VulkanHooks::hkvkGetDeviceQueue2(
    VkDevice device,
    const VkDeviceQueueInfo2* queue_info,
    VkQueue* queue) {
    VulkanDeviceState state{};
    PFN_vkGetDeviceQueue2 get_queue = nullptr;
    if (VulkanDeviceRegistry::get_state(device, &state))
        get_queue = state.get_device_queue2;
    if (!get_queue)
        get_queue = o_vkGetDeviceQueue2;

    if (!get_queue || !queue_info || !queue)
        return;

    get_queue(device, queue_info, queue);
    if (*queue != VK_NULL_HANDLE)
        VulkanDeviceRegistry::register_queue(device, *queue);
}

VkResult VKAPI_CALL VulkanHooks::hkvkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* image_index) {
    PFN_vkAcquireNextImageKHR acquire = VulkanDeviceRegistry::get_acquire_next_image(device);
    if (!acquire)
        acquire = o_vkAcquireNextImageKHR;
    if (!acquire)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult result = acquire(device, swapchain, timeout, semaphore, fence, image_index);
    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && image_index) {
        if (auto* low_latency = LowLatencyCtx::get())
            (void)low_latency->VulkanFlexOnAcquire(device, swapchain, *image_index);
    }
    return result;
}

VkResult VKAPI_CALL VulkanHooks::hkvkAcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* acquire_info,
    uint32_t* image_index) {
    PFN_vkAcquireNextImage2KHR acquire = VulkanDeviceRegistry::get_acquire_next_image2(device);
    if (!acquire)
        acquire = o_vkAcquireNextImage2KHR;
    if (!acquire)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    const VkResult result = acquire(device, acquire_info, image_index);
    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && acquire_info && image_index) {
        if (auto* low_latency = LowLatencyCtx::get())
            (void)low_latency->VulkanFlexOnAcquire(device, acquire_info->swapchain, *image_index);
    }
    return result;
}

VkResult VKAPI_CALL VulkanHooks::hkvkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* create_info,
    const VkAllocationCallbacks* allocator,
    VkSwapchainKHR* swapchain) {
    VulkanDeviceState state{};
    PFN_vkCreateSwapchainKHR create_swapchain = nullptr;
    if (VulkanDeviceRegistry::get_state(device, &state))
        create_swapchain = state.create_swapchain;
    if (!create_swapchain)
        create_swapchain = o_vkCreateSwapchainKHR;
    if (!create_swapchain || !create_info || !swapchain)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult result = create_swapchain(device, create_info, allocator, swapchain);
    if (result == VK_SUCCESS && *swapchain != VK_NULL_HANDLE) {
        (void)VulkanDeviceRegistry::register_swapchain(device, *swapchain, create_info->flags);
    }
    return result;
}

VkResult VKAPI_CALL VulkanHooks::hkvkCreateGraphicsPipelines(
    VkDevice device,
    VkPipelineCache pipeline_cache,
    std::uint32_t create_info_count,
    const VkGraphicsPipelineCreateInfo* create_infos,
    const VkAllocationCallbacks* allocator,
    VkPipeline* pipelines) {
    VulkanDeviceState state{};
    PFN_vkCreateGraphicsPipelines create_pipelines = nullptr;
    if (VulkanDeviceRegistry::get_state(device, &state))
        create_pipelines = state.create_graphics_pipelines;
    if (!create_pipelines && o_vkGetDeviceProcAddr)
        create_pipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
            o_vkGetDeviceProcAddr(device, "vkCreateGraphicsPipelines"));
    if (!create_pipelines)
        return VK_ERROR_INITIALIZATION_FAILED;

    const auto start_ns = get_timestamp();
    const VkResult result = create_pipelines(
        device, pipeline_cache, create_info_count, create_infos, allocator, pipelines);
    const auto end_ns = get_timestamp();

    const auto& vf = Config::get().snapshot().vulkanflex;
    if (vf.structural_stall != policy::AutoBool::Disabled && end_ns > start_ns &&
        end_ns - start_ns >= static_cast<std::uint64_t>(vf.pipeline_threshold_us) * 1000ull) {
        if (auto* low_latency = LowLatencyCtx::get())
            low_latency->VulkanFlexOnStructuralStall(
                device, StructuralStallKind::GraphicsPipeline, end_ns - start_ns, end_ns);
    }
    return result;
}

VkResult VKAPI_CALL VulkanHooks::hkvkCreateComputePipelines(
    VkDevice device,
    VkPipelineCache pipeline_cache,
    std::uint32_t create_info_count,
    const VkComputePipelineCreateInfo* create_infos,
    const VkAllocationCallbacks* allocator,
    VkPipeline* pipelines) {
    VulkanDeviceState state{};
    PFN_vkCreateComputePipelines create_pipelines = nullptr;
    if (VulkanDeviceRegistry::get_state(device, &state))
        create_pipelines = state.create_compute_pipelines;
    if (!create_pipelines && o_vkGetDeviceProcAddr)
        create_pipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(
            o_vkGetDeviceProcAddr(device, "vkCreateComputePipelines"));
    if (!create_pipelines)
        return VK_ERROR_INITIALIZATION_FAILED;

    const auto start_ns = get_timestamp();
    const VkResult result = create_pipelines(
        device, pipeline_cache, create_info_count, create_infos, allocator, pipelines);
    const auto end_ns = get_timestamp();

    const auto& vf = Config::get().snapshot().vulkanflex;
    if (vf.structural_stall != policy::AutoBool::Disabled && end_ns > start_ns &&
        end_ns - start_ns >= static_cast<std::uint64_t>(vf.pipeline_threshold_us) * 1000ull) {
        if (auto* low_latency = LowLatencyCtx::get())
            low_latency->VulkanFlexOnStructuralStall(
                device, StructuralStallKind::ComputePipeline, end_ns - start_ns, end_ns);
    }
    return result;
}

VkResult VKAPI_CALL VulkanHooks::hkvkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* present_info) {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkQueuePresentKHR present = nullptr;
    (void)VulkanDeviceRegistry::get_queue_present_dispatch(queue, &device, &present);
    if (!present)
        present = o_vkQueuePresentKHR;
    if (!present)
        return VK_ERROR_INITIALIZATION_FAILED;

    struct ClaimedPresent {
        std::uint32_t present_index = 0;
        VulkanFlex::PresentTicket ticket{};
    };
    std::array<ClaimedPresent, VulkanFlex::kTrackedSwapchainLanes> claims{};
    std::size_t claim_count = 0;

    // Claim image->frame ownership before entering the real present. A fast
    // acquire on another thread may recycle an image immediately after the
    // driver's call returns, so post-call claiming is too late. The fixed array
    // matches VulkanFlex's bounded swapchain-lane capacity and never allocates.
    auto* low_latency = (present_info && device != VK_NULL_HANDLE &&
                         present_info->pSwapchains && present_info->pImageIndices)
        ? LowLatencyCtx::get() : nullptr;
    if (low_latency) {
        for (std::uint32_t i = 0; i < present_info->swapchainCount && claim_count < claims.size(); ++i) {
            auto ticket = low_latency->VulkanFlexBeginPresent(
                device, present_info->pSwapchains[i], present_info->pImageIndices[i]);
            if (ticket)
                claims[claim_count++] = ClaimedPresent{i, ticket};
        }
    }

    const VkResult result = present(queue, present_info);
    if (!present_info || device == VK_NULL_HANDLE || claim_count == 0 ||
        (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)) {
        return result;
    }

    const auto metadata_view = parse_present_metadata(*present_info);
    for (std::size_t claim = 0; claim < claim_count; ++claim) {
        const auto index = claims[claim].present_index;
        if (present_info->pResults &&
            present_info->pResults[index] != VK_SUCCESS &&
            present_info->pResults[index] != VK_SUBOPTIMAL_KHR) {
            continue;
        }
        const auto metadata = present_metadata_at(*present_info, metadata_view, index);
        low_latency->VulkanFlexCompletePresent(
            device,
            claims[claim].ticket,
            metadata.present_id,
            metadata.uses_present_id2,
            metadata.timing_requested);
    }
    return result;
}

void VKAPI_CALL VulkanHooks::hkvkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator) {
    VulkanDeviceState state{};
    PFN_vkDestroySwapchainKHR destroy = nullptr;
    if (VulkanDeviceRegistry::get_state(device, &state))
        destroy = state.destroy_swapchain;
    if (!destroy)
        destroy = o_vkDestroySwapchainKHR;

    if (auto* low_latency = LowLatencyCtx::get())
        low_latency->VulkanFlexOnDestroySwapchain(device, swapchain);

    VulkanDeviceRegistry::unregister_swapchain(swapchain);
    if (destroy)
        destroy(device, swapchain, allocator);
}

PFN_vkVoidFunction VKAPI_CALL VulkanHooks::hkvkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name)
        return nullptr;

    VulkanDeviceState state{};
    const bool tracked = VulkanDeviceRegistry::get_state(device, &state);

    if (tracked && state.nv_low_latency2_enabled) {
        if (const auto hook = nv_low_latency2_hook_by_name(name))
            return hook;
    }

    if (tracked && std::strcmp(name, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkGetDeviceQueue);
    if (tracked && std::strcmp(name, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkGetDeviceQueue2);
    if (tracked && std::strcmp(name, "vkAcquireNextImageKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkAcquireNextImageKHR);
    if (tracked && std::strcmp(name, "vkAcquireNextImage2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkAcquireNextImage2KHR);
    if (tracked && std::strcmp(name, "vkCreateSwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkCreateSwapchainKHR);
    const bool structural_stall_hooks =
        Config::get().snapshot().vulkanflex.structural_stall != policy::AutoBool::Disabled;
    if (structural_stall_hooks && tracked && state.create_graphics_pipelines &&
        std::strcmp(name, "vkCreateGraphicsPipelines") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkCreateGraphicsPipelines);
    if (structural_stall_hooks && tracked && state.create_compute_pipelines &&
        std::strcmp(name, "vkCreateComputePipelines") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkCreateComputePipelines);
    if (tracked && std::strcmp(name, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkQueuePresentKHR);
    if (tracked && std::strcmp(name, "vkDestroySwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkDestroySwapchainKHR);

    return o_vkGetDeviceProcAddr ? o_vkGetDeviceProcAddr(device, name) : nullptr;
}

PFN_vkVoidFunction VKAPI_CALL VulkanHooks::hkvkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name)
        return nullptr;

    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkGetInstanceProcAddr);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkGetDeviceProcAddr);
    if (std::strcmp(name, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkCreateDevice);
    if (std::strcmp(name, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkDestroyDevice);
    if (std::strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkEnumerateDeviceExtensionProperties);
    if (std::strcmp(name, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkGetDeviceQueue);
    if (std::strcmp(name, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkGetDeviceQueue2);
    if (std::strcmp(name, "vkAcquireNextImageKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkAcquireNextImageKHR);
    if (std::strcmp(name, "vkAcquireNextImage2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkAcquireNextImage2KHR);
    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkCreateSwapchainKHR);
    const bool structural_stall_hooks =
        Config::get().snapshot().vulkanflex.structural_stall != policy::AutoBool::Disabled;
    if (structural_stall_hooks && std::strcmp(name, "vkCreateGraphicsPipelines") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkCreateGraphicsPipelines);
    if (structural_stall_hooks && std::strcmp(name, "vkCreateComputePipelines") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkCreateComputePipelines);
    if (std::strcmp(name, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkQueuePresentKHR);
    if (std::strcmp(name, "vkDestroySwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hkvkDestroySwapchainKHR);

    const auto& snapshot = Config::get().snapshot();
    if (snapshot.vulkan.expose_nv_low_latency2 != policy::AutoBool::Disabled && is_nv_low_latency2_name(name)) {
        if (const auto hook = nv_low_latency2_hook_by_name(name))
            return hook;
    }

    return o_vkGetInstanceProcAddr ? o_vkGetInstanceProcAddr(instance, name) : nullptr;
}

VkResult VKAPI_CALL VulkanHooks::hkvkSetLatencySleepModeNV(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkLatencySleepModeInfoNV* sleep_mode_info) {
    VulkanNvLowLatency2Dispatch dispatch{};
    if (!VulkanDeviceRegistry::get_nv_low_latency2_dispatch(device, &dispatch))
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    if (dispatch.native && dispatch.set_sleep_mode)
        return dispatch.set_sleep_mode(device, swapchain, sleep_mode_info);

    SleepMode mode{};
    if (sleep_mode_info) {
        mode.low_latency_enabled = sleep_mode_info->lowLatencyMode == VK_TRUE;
        mode.low_latency_boost = sleep_mode_info->lowLatencyBoost == VK_TRUE;
        mode.minimum_interval_us = sleep_mode_info->minimumIntervalUs;
        mode.use_markers_to_optimize = true;
    }

    auto* low_latency = LowLatencyCtx::get();
    if (!low_latency)
        return VK_ERROR_INITIALIZATION_FAILED;
    return low_latency->VulkanFrontendSetSleepMode(
        policy::InputFrontend::VkNvLowLatency2,
        device,
        mode) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult VKAPI_CALL VulkanHooks::hkvkLatencySleepNV(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkLatencySleepInfoNV* sleep_info) {
    if (!sleep_info)
        return VK_ERROR_INITIALIZATION_FAILED;

    VulkanNvLowLatency2Dispatch dispatch{};
    if (!VulkanDeviceRegistry::get_nv_low_latency2_dispatch(device, &dispatch))
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    auto* low_latency = LowLatencyCtx::get();
    if (!low_latency) {
        // During teardown or partial initialization, preserve native driver
        // semantics when the real extension is available. Emulation remains
        // fail-open by satisfying the application's timeline dependency.
        if (dispatch.native && dispatch.latency_sleep)
            return dispatch.latency_sleep(device, swapchain, sleep_info);
        return VulkanDeviceRegistry::signal_semaphore(
            device, sleep_info->signalSemaphore, sleep_info->value);
    }

    if (dispatch.native && dispatch.latency_sleep) {
        const bool selected = low_latency->VulkanFrontendObserveSleep(
            policy::InputFrontend::VkNvLowLatency2);
        if (selected)
            return dispatch.latency_sleep(device, swapchain, sleep_info);

        // Another frontend won arbitration. Preserve the application's timeline
        // dependency without invoking a second pacing implementation.
        return VulkanDeviceRegistry::signal_semaphore(
            device,
            sleep_info->signalSemaphore,
            sleep_info->value);
    }

    const bool selected = low_latency->VulkanFrontendObserveSleep(
        policy::InputFrontend::VkNvLowLatency2);
    if (!selected) {
        return VulkanDeviceRegistry::signal_semaphore(
            device,
            sleep_info->signalSemaphore,
            sleep_info->value);
    }

    // VK_NV_low_latency2 requires vkLatencySleepNV to return immediately. The
    // worker performs the selected backend's pacing and signals the caller's
    // timeline semaphore afterwards. If the fixed queue is saturated, fail
    // open by signalling immediately rather than stalling the application.
    if (enqueue_emulated_sleep(device, *sleep_info))
        return VK_SUCCESS;

    return VulkanDeviceRegistry::signal_semaphore(
        device,
        sleep_info->signalSemaphore,
        sleep_info->value);
}

void VKAPI_CALL VulkanHooks::hkvkSetLatencyMarkerNV(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkSetLatencyMarkerInfoNV* marker_info) {
    if (!marker_info)
        return;

    VulkanNvLowLatency2Dispatch dispatch{};
    if (!VulkanDeviceRegistry::get_nv_low_latency2_dispatch(device, &dispatch))
        return;

    const auto marker = marker_from_vk(marker_info->marker);
    if (!marker.has_value()) {
        if (dispatch.native && dispatch.set_marker)
            dispatch.set_marker(device, swapchain, marker_info);
        return;
    }

    auto* low_latency = LowLatencyCtx::get();
    if (!low_latency) {
        if (dispatch.native && dispatch.set_marker)
            dispatch.set_marker(device, swapchain, marker_info);
        return;
    }

    if (dispatch.native && dispatch.set_marker) {
        const bool selected = low_latency->VulkanFrontendObserveMarker(
            policy::InputFrontend::VkNvLowLatency2,
            *marker,
            marker_info->presentID);
        if (selected)
            dispatch.set_marker(device, swapchain, marker_info);
        return;
    }

    low_latency->VulkanFrontendSetMarker(
        policy::InputFrontend::VkNvLowLatency2,
        device,
        *marker,
        marker_info->presentID);
}

void VKAPI_CALL VulkanHooks::hkvkGetLatencyTimingsNV(
    VkDevice device,
    VkSwapchainKHR swapchain,
    VkGetLatencyMarkerInfoNV* marker_info) {
    if (!marker_info)
        return;

    VulkanNvLowLatency2Dispatch dispatch{};
    if (!VulkanDeviceRegistry::get_nv_low_latency2_dispatch(device, &dispatch))
        return;

    if (dispatch.native && dispatch.get_timings) {
        dispatch.get_timings(device, swapchain, marker_info);
        return;
    }

    if (auto* low_latency = LowLatencyCtx::get())
        low_latency->VulkanFrontendGetLatencyTimings(marker_info);
}

void VKAPI_CALL VulkanHooks::hkvkQueueNotifyOutOfBandNV(
    VkQueue queue,
    const VkOutOfBandQueueTypeInfoNV* queue_type_info) {
    if (!queue_type_info)
        return;

    if (const auto native = VulkanDeviceRegistry::get_native_queue_notify(queue))
        native(queue, queue_type_info);
    // Emulated output backends consume the explicit out-of-band latency
    // markers, so no additional queue registration is required.
}

void VulkanHooks::hook_vulkan(HMODULE vulkan_module) {
    if (!vulkan_module)
        return;

    std::scoped_lock lock(g_hook_mutex);
    if (g_vulkan_hooks_installed && g_hooked_vulkan_module == vulkan_module)
        return;

    if (g_vulkan_hooks_installed) {
        spdlog::warn("A different vulkan-1.dll module was observed after hooks were installed; keeping the first loader");
        return;
    }

    spdlog::debug("Trying to hook Vulkan");

    o_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(vulkan_module, "vkGetInstanceProcAddr"));
    o_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        GetProcAddress(vulkan_module, "vkGetDeviceProcAddr"));
    o_vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
        GetProcAddress(vulkan_module, "vkCreateDevice"));
    o_vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
        GetProcAddress(vulkan_module, "vkDestroyDevice"));
    o_vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        GetProcAddress(vulkan_module, "vkGetPhysicalDeviceFeatures2"));
    o_vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        GetProcAddress(vulkan_module, "vkGetPhysicalDeviceProperties"));
    if (!o_vkGetPhysicalDeviceFeatures2) {
        o_vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            GetProcAddress(vulkan_module, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    o_vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        GetProcAddress(vulkan_module, "vkEnumerateDeviceExtensionProperties"));
    o_vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
        GetProcAddress(vulkan_module, "vkGetDeviceQueue"));
    o_vkGetDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
        GetProcAddress(vulkan_module, "vkGetDeviceQueue2"));
    o_vkAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
        GetProcAddress(vulkan_module, "vkAcquireNextImageKHR"));
    o_vkAcquireNextImage2KHR = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(
        GetProcAddress(vulkan_module, "vkAcquireNextImage2KHR"));
    o_vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        GetProcAddress(vulkan_module, "vkCreateSwapchainKHR"));
    o_vkQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
        GetProcAddress(vulkan_module, "vkQueuePresentKHR"));
    o_vkDestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        GetProcAddress(vulkan_module, "vkDestroySwapchainKHR"));

    if (!o_vkCreateDevice || !o_vkGetDeviceProcAddr ||
        !o_vkEnumerateDeviceExtensionProperties || !o_vkGetPhysicalDeviceProperties) {
        spdlog::warn("Vulkan hooks unavailable: loader is missing required core entry points");
        return;
    }

    const LONG begin_result = DetourTransactionBegin();
    if (begin_result != NO_ERROR) {
        spdlog::error("DetourTransactionBegin failed for Vulkan hooks: {}", begin_result);
        return;
    }

    DetourUpdateThread(GetCurrentThread());

    if (o_vkGetInstanceProcAddr) {
        if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkGetInstanceProcAddr), reinterpret_cast<PVOID>(hkvkGetInstanceProcAddr)) == NO_ERROR)
            g_vk_gipa_attached = true;
    }
    if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkGetDeviceProcAddr), reinterpret_cast<PVOID>(hkvkGetDeviceProcAddr)) == NO_ERROR)
        g_vk_gdpa_attached = true;
    if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkCreateDevice), reinterpret_cast<PVOID>(hkvkCreateDevice)) == NO_ERROR)
        g_vk_create_device_attached = true;
    if (o_vkDestroyDevice) {
        if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkDestroyDevice), reinterpret_cast<PVOID>(hkvkDestroyDevice)) == NO_ERROR)
            g_vk_destroy_device_attached = true;
    }
    if (DetourAttach(
            reinterpret_cast<PVOID*>(&o_vkEnumerateDeviceExtensionProperties),
            reinterpret_cast<PVOID>(hkvkEnumerateDeviceExtensionProperties)) == NO_ERROR) {
        g_vk_enum_extensions_attached = true;
    }
    if (o_vkGetDeviceQueue) {
        if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkGetDeviceQueue), reinterpret_cast<PVOID>(hkvkGetDeviceQueue)) == NO_ERROR)
            g_vk_get_device_queue_attached = true;
    }
    if (o_vkGetDeviceQueue2) {
        if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkGetDeviceQueue2), reinterpret_cast<PVOID>(hkvkGetDeviceQueue2)) == NO_ERROR)
            g_vk_get_device_queue2_attached = true;
    }
    if (o_vkAcquireNextImageKHR) {
        if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkAcquireNextImageKHR), reinterpret_cast<PVOID>(hkvkAcquireNextImageKHR)) == NO_ERROR)
            g_vk_acquire_next_image_attached = true;
    }
    if (o_vkAcquireNextImage2KHR) {
        if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkAcquireNextImage2KHR), reinterpret_cast<PVOID>(hkvkAcquireNextImage2KHR)) == NO_ERROR)
            g_vk_acquire_next_image2_attached = true;
    }
    if (o_vkCreateSwapchainKHR) {
        if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkCreateSwapchainKHR), reinterpret_cast<PVOID>(hkvkCreateSwapchainKHR)) == NO_ERROR)
            g_vk_create_swapchain_attached = true;
    }
    if (o_vkQueuePresentKHR) {
        if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkQueuePresentKHR), reinterpret_cast<PVOID>(hkvkQueuePresentKHR)) == NO_ERROR)
            g_vk_queue_present_attached = true;
    }
    if (o_vkDestroySwapchainKHR) {
        if (DetourAttach(reinterpret_cast<PVOID*>(&o_vkDestroySwapchainKHR), reinterpret_cast<PVOID>(hkvkDestroySwapchainKHR)) == NO_ERROR)
            g_vk_destroy_swapchain_attached = true;
    }

    const LONG commit_result = DetourTransactionCommit();
    if (commit_result != NO_ERROR) {
        g_vk_gipa_attached = false;
        g_vk_gdpa_attached = false;
        g_vk_create_device_attached = false;
        g_vk_destroy_device_attached = false;
        g_vk_enum_extensions_attached = false;
        g_vk_get_device_queue_attached = false;
        g_vk_get_device_queue2_attached = false;
        g_vk_acquire_next_image_attached = false;
        g_vk_acquire_next_image2_attached = false;
        g_vk_create_swapchain_attached = false;
        g_vk_queue_present_attached = false;
        g_vk_destroy_swapchain_attached = false;
        spdlog::error("DetourTransactionCommit failed for Vulkan hooks: {}", commit_result);
        return;
    }

    g_vulkan_hooks_installed = true;
    g_hooked_vulkan_module = vulkan_module;
    transport_truth::TransportTruth::note_native_vulkan_hooks_installed();
    spdlog::info("Vulkan hooks installed (low-latency frontends + VulkanFlex WSI bridge)");
}
