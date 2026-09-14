#include "vulkan_device_registry.h"

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <mutex>

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace {
constexpr size_t kMaxTrackedVulkanDevices = 16;
constexpr size_t kMaxTrackedVulkanQueues = 64;
constexpr size_t kMaxTrackedVulkanSwapchains = 64;

constexpr std::uint32_t kFlagAmdAntiLag = 1u << 0;
constexpr std::uint32_t kFlagNvLowLatency2 = 1u << 1;
constexpr std::uint32_t kFlagNvLowLatency2Native = 1u << 2;
constexpr std::uint32_t kFlagPresentId = 1u << 3;
constexpr std::uint32_t kFlagPresentId2 = 1u << 4;
constexpr std::uint32_t kFlagPresentWait = 1u << 5;
constexpr std::uint32_t kFlagPresentWait2 = 1u << 6;
constexpr std::uint32_t kFlagPresentTiming = 1u << 7;
constexpr std::uint32_t kFlagCapabilityProbeComplete = 1u << 8;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free on the supported x86-64 target");
static_assert(std::atomic<VkDevice>::is_always_lock_free,
              "Vulkan handle atomics must be lock-free on the supported x86-64 target");
static_assert(std::atomic<PFN_vkVoidFunction>::is_always_lock_free,
              "Vulkan dispatch-pointer atomics must be lock-free on the supported x86-64 target");

// The public VulkanDeviceState is deliberately plain/trivially-copyable. The
// registry stores every field atomically so readers can stay entirely outside
// the mutation mutex. A single odd/even generation invalidates TLS snapshots
// whenever a rare topology mutation occurs.
struct AtomicDeviceSlot {
    std::atomic<VkDevice> device{VK_NULL_HANDLE};

    std::atomic<PFN_vkCreateSemaphore> create_semaphore{nullptr};
    std::atomic<PFN_vkDestroySemaphore> destroy_semaphore{nullptr};
    std::atomic<PFN_vkSignalSemaphore> signal_semaphore{nullptr};
    std::atomic<PFN_vkAntiLagUpdateAMD> anti_lag_update{nullptr};
    std::atomic<PFN_vkGetDeviceQueue> get_device_queue{nullptr};
    std::atomic<PFN_vkGetDeviceQueue2> get_device_queue2{nullptr};
    std::atomic<PFN_vkAcquireNextImageKHR> acquire_next_image{nullptr};
    std::atomic<PFN_vkAcquireNextImage2KHR> acquire_next_image2{nullptr};
    std::atomic<PFN_vkCreateSwapchainKHR> create_swapchain{nullptr};
    std::atomic<PFN_vkQueuePresentKHR> queue_present{nullptr};
    std::atomic<PFN_vkDestroySwapchainKHR> destroy_swapchain{nullptr};
    std::atomic<PFN_vkCreateGraphicsPipelines> create_graphics_pipelines{nullptr};
    std::atomic<PFN_vkCreateComputePipelines> create_compute_pipelines{nullptr};
    std::atomic<PFN_vkWaitForPresentKHR> wait_for_present{nullptr};
    std::atomic<PFN_vkWaitForPresent2KHR> wait_for_present2{nullptr};
    std::atomic<PFN_vkGetPastPresentationTimingEXT> get_past_presentation_timing{nullptr};

    std::atomic<PFN_vkSetLatencySleepModeNV> nv_set_latency_sleep_mode{nullptr};
    std::atomic<PFN_vkLatencySleepNV> nv_latency_sleep{nullptr};
    std::atomic<PFN_vkSetLatencyMarkerNV> nv_set_latency_marker{nullptr};
    std::atomic<PFN_vkGetLatencyTimingsNV> nv_get_latency_timings{nullptr};
    std::atomic<PFN_vkQueueNotifyOutOfBandNV> nv_queue_notify_out_of_band{nullptr};

    std::atomic<VkSemaphore> low_latency_semaphore{VK_NULL_HANDLE};
    std::atomic<std::uint32_t> flags{0};

    std::atomic<std::uint32_t> capability_api_version{0};
    std::atomic<std::uint32_t> capability_driver_version{0};
    std::atomic<std::uint32_t> capability_vendor_id{0};
    std::atomic<std::uint32_t> capability_device_id{0};
    std::atomic<std::uint64_t> capability_supported{0};
    std::atomic<std::uint64_t> capability_enabled{0};
    std::atomic<std::uint64_t> capability_enabled_known{0};
    std::atomic<std::uint64_t> capability_eds3_supported{0};
    std::atomic<std::uint64_t> capability_eds3_enabled{0};
    std::atomic<std::uint64_t> capability_eds3_enabled_known{0};
};

struct AtomicQueueSlot {
    std::atomic<VkQueue> queue{VK_NULL_HANDLE};
    std::atomic<VkDevice> device{VK_NULL_HANDLE};
    // Queue-local dispatch is cached at queue-registration time. Frame-hot
    // present/OOB callbacks therefore avoid queue -> device -> device-state
    // pointer chasing and avoid copying the full VulkanDeviceState.
    std::atomic<PFN_vkQueuePresentKHR> queue_present{nullptr};
    std::atomic<PFN_vkQueueNotifyOutOfBandNV> native_notify{nullptr};
};

struct AtomicSwapchainSlot {
    std::atomic<VkSwapchainKHR> swapchain{VK_NULL_HANDLE};
    std::atomic<VkDevice> device{VK_NULL_HANDLE};
    std::atomic<VkSwapchainCreateFlagsKHR> flags{0};
};

std::array<AtomicDeviceSlot, kMaxTrackedVulkanDevices> g_devices{};
std::array<AtomicQueueSlot, kMaxTrackedVulkanQueues> g_queues{};
std::array<AtomicSwapchainSlot, kMaxTrackedVulkanSwapchains> g_swapchains{};
std::mutex g_writer_mutex;

// Even = stable snapshot, odd = a writer is publishing a new topology.
std::atomic<std::uint64_t> g_generation{2};

struct MutationScope {
    MutationScope() noexcept {
        g_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    ~MutationScope() {
        g_generation.fetch_add(1, std::memory_order_release);
    }
    MutationScope(const MutationScope&) = delete;
    MutationScope& operator=(const MutationScope&) = delete;
};

struct ThreadLocalDeviceCache {
    VkDevice device = VK_NULL_HANDLE;
    std::uint64_t generation = 0;
    bool valid = false;
    VulkanDeviceState state{};
};

struct ThreadLocalQueueCache {
    VkQueue queue = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkQueuePresentKHR queue_present = nullptr;
    PFN_vkQueueNotifyOutOfBandNV native_notify = nullptr;
    std::uint64_t generation = 0;
    bool valid = false;
    bool fallback_resolved = false;
};

thread_local ThreadLocalDeviceCache g_tls_device{};
thread_local ThreadLocalQueueCache g_tls_queue{};

inline void cpu_relax() noexcept {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    _mm_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

std::uint64_t stable_generation() noexcept {
    for (;;) {
        const auto generation = g_generation.load(std::memory_order_acquire);
        if ((generation & 1u) == 0)
            return generation;
        // Topology mutations are rare, but when a reader collides with one do
        // not hammer the shared generation cache line at full issue rate. PAUSE
        // is the correct x86 spin-wait hint and does not touch the steady path.
        cpu_relax();
    }
}

std::uint32_t pack_flags(const VulkanDeviceState& state) noexcept {
    std::uint32_t flags = 0;
    if (state.amd_anti_lag_enabled) flags |= kFlagAmdAntiLag;
    if (state.nv_low_latency2_enabled) flags |= kFlagNvLowLatency2;
    if (state.nv_low_latency2_native) flags |= kFlagNvLowLatency2Native;
    if (state.present_id_enabled) flags |= kFlagPresentId;
    if (state.present_id2_enabled) flags |= kFlagPresentId2;
    if (state.present_wait_enabled) flags |= kFlagPresentWait;
    if (state.present_wait2_enabled) flags |= kFlagPresentWait2;
    if (state.present_timing_enabled) flags |= kFlagPresentTiming;
    if (state.capabilities.support_probe_complete) flags |= kFlagCapabilityProbeComplete;
    return flags;
}

void publish_device_slot(AtomicDeviceSlot& slot, const VulkanDeviceState& state) noexcept {
    // g_generation is odd while this executes, so no new TLS snapshot will be
    // accepted until all fields and the identity have been published.
    slot.device.store(VK_NULL_HANDLE, std::memory_order_release);
    slot.create_semaphore.store(state.create_semaphore, std::memory_order_relaxed);
    slot.destroy_semaphore.store(state.destroy_semaphore, std::memory_order_relaxed);
    slot.signal_semaphore.store(state.signal_semaphore, std::memory_order_relaxed);
    slot.anti_lag_update.store(state.anti_lag_update, std::memory_order_relaxed);
    slot.get_device_queue.store(state.get_device_queue, std::memory_order_relaxed);
    slot.get_device_queue2.store(state.get_device_queue2, std::memory_order_relaxed);
    slot.acquire_next_image.store(state.acquire_next_image, std::memory_order_relaxed);
    slot.acquire_next_image2.store(state.acquire_next_image2, std::memory_order_relaxed);
    slot.create_swapchain.store(state.create_swapchain, std::memory_order_relaxed);
    slot.queue_present.store(state.queue_present, std::memory_order_relaxed);
    slot.destroy_swapchain.store(state.destroy_swapchain, std::memory_order_relaxed);
    slot.create_graphics_pipelines.store(state.create_graphics_pipelines, std::memory_order_relaxed);
    slot.create_compute_pipelines.store(state.create_compute_pipelines, std::memory_order_relaxed);
    slot.wait_for_present.store(state.wait_for_present, std::memory_order_relaxed);
    slot.wait_for_present2.store(state.wait_for_present2, std::memory_order_relaxed);
    slot.get_past_presentation_timing.store(state.get_past_presentation_timing, std::memory_order_relaxed);
    slot.nv_set_latency_sleep_mode.store(state.nv_set_latency_sleep_mode, std::memory_order_relaxed);
    slot.nv_latency_sleep.store(state.nv_latency_sleep, std::memory_order_relaxed);
    slot.nv_set_latency_marker.store(state.nv_set_latency_marker, std::memory_order_relaxed);
    slot.nv_get_latency_timings.store(state.nv_get_latency_timings, std::memory_order_relaxed);
    slot.nv_queue_notify_out_of_band.store(state.nv_queue_notify_out_of_band, std::memory_order_relaxed);
    slot.low_latency_semaphore.store(state.low_latency_semaphore, std::memory_order_relaxed);
    slot.capability_api_version.store(state.capabilities.api_version, std::memory_order_relaxed);
    slot.capability_driver_version.store(state.capabilities.driver_version, std::memory_order_relaxed);
    slot.capability_vendor_id.store(state.capabilities.vendor_id, std::memory_order_relaxed);
    slot.capability_device_id.store(state.capabilities.device_id, std::memory_order_relaxed);
    slot.capability_supported.store(state.capabilities.supported, std::memory_order_relaxed);
    slot.capability_enabled.store(state.capabilities.enabled, std::memory_order_relaxed);
    slot.capability_enabled_known.store(state.capabilities.enabled_known, std::memory_order_relaxed);
    slot.capability_eds3_supported.store(state.capabilities.eds3_supported, std::memory_order_relaxed);
    slot.capability_eds3_enabled.store(state.capabilities.eds3_enabled, std::memory_order_relaxed);
    slot.capability_eds3_enabled_known.store(state.capabilities.eds3_enabled_known, std::memory_order_relaxed);
    slot.flags.store(pack_flags(state), std::memory_order_relaxed);
    slot.device.store(state.device, std::memory_order_release);
}

void clear_device_slot(AtomicDeviceSlot& slot) noexcept {
    slot.device.store(VK_NULL_HANDLE, std::memory_order_release);
    slot.create_semaphore.store(nullptr, std::memory_order_relaxed);
    slot.destroy_semaphore.store(nullptr, std::memory_order_relaxed);
    slot.signal_semaphore.store(nullptr, std::memory_order_relaxed);
    slot.anti_lag_update.store(nullptr, std::memory_order_relaxed);
    slot.get_device_queue.store(nullptr, std::memory_order_relaxed);
    slot.get_device_queue2.store(nullptr, std::memory_order_relaxed);
    slot.acquire_next_image.store(nullptr, std::memory_order_relaxed);
    slot.acquire_next_image2.store(nullptr, std::memory_order_relaxed);
    slot.create_swapchain.store(nullptr, std::memory_order_relaxed);
    slot.queue_present.store(nullptr, std::memory_order_relaxed);
    slot.destroy_swapchain.store(nullptr, std::memory_order_relaxed);
    slot.create_graphics_pipelines.store(nullptr, std::memory_order_relaxed);
    slot.create_compute_pipelines.store(nullptr, std::memory_order_relaxed);
    slot.wait_for_present.store(nullptr, std::memory_order_relaxed);
    slot.wait_for_present2.store(nullptr, std::memory_order_relaxed);
    slot.get_past_presentation_timing.store(nullptr, std::memory_order_relaxed);
    slot.nv_set_latency_sleep_mode.store(nullptr, std::memory_order_relaxed);
    slot.nv_latency_sleep.store(nullptr, std::memory_order_relaxed);
    slot.nv_set_latency_marker.store(nullptr, std::memory_order_relaxed);
    slot.nv_get_latency_timings.store(nullptr, std::memory_order_relaxed);
    slot.nv_queue_notify_out_of_band.store(nullptr, std::memory_order_relaxed);
    slot.low_latency_semaphore.store(VK_NULL_HANDLE, std::memory_order_relaxed);
    slot.capability_api_version.store(0, std::memory_order_relaxed);
    slot.capability_driver_version.store(0, std::memory_order_relaxed);
    slot.capability_vendor_id.store(0, std::memory_order_relaxed);
    slot.capability_device_id.store(0, std::memory_order_relaxed);
    slot.capability_supported.store(0, std::memory_order_relaxed);
    slot.capability_enabled.store(0, std::memory_order_relaxed);
    slot.capability_enabled_known.store(0, std::memory_order_relaxed);
    slot.capability_eds3_supported.store(0, std::memory_order_relaxed);
    slot.capability_eds3_enabled.store(0, std::memory_order_relaxed);
    slot.capability_eds3_enabled_known.store(0, std::memory_order_relaxed);
    slot.flags.store(0, std::memory_order_relaxed);
}

AtomicDeviceSlot* find_device_slot_writer(VkDevice device) noexcept {
    for (auto& slot : g_devices) {
        if (slot.device.load(std::memory_order_relaxed) == device)
            return &slot;
    }
    return nullptr;
}

AtomicDeviceSlot* find_free_device_slot_writer() noexcept {
    for (auto& slot : g_devices) {
        if (slot.device.load(std::memory_order_relaxed) == VK_NULL_HANDLE)
            return &slot;
    }
    return nullptr;
}

AtomicQueueSlot* find_queue_slot_writer(VkQueue queue) noexcept {
    for (auto& slot : g_queues) {
        if (slot.queue.load(std::memory_order_relaxed) == queue)
            return &slot;
    }
    return nullptr;
}

AtomicQueueSlot* find_free_queue_slot_writer() noexcept {
    for (auto& slot : g_queues) {
        if (slot.queue.load(std::memory_order_relaxed) == VK_NULL_HANDLE)
            return &slot;
    }
    return nullptr;
}

AtomicSwapchainSlot* find_swapchain_slot_writer(VkSwapchainKHR swapchain) noexcept {
    for (auto& slot : g_swapchains) {
        if (slot.swapchain.load(std::memory_order_relaxed) == swapchain)
            return &slot;
    }
    return nullptr;
}

AtomicSwapchainSlot* find_free_swapchain_slot_writer() noexcept {
    for (auto& slot : g_swapchains) {
        if (slot.swapchain.load(std::memory_order_relaxed) == VK_NULL_HANDLE)
            return &slot;
    }
    return nullptr;
}

void erase_swapchains_for_device_writer(VkDevice device) noexcept {
    for (auto& slot : g_swapchains) {
        if (slot.device.load(std::memory_order_relaxed) == device) {
            slot.swapchain.store(VK_NULL_HANDLE, std::memory_order_release);
            slot.device.store(VK_NULL_HANDLE, std::memory_order_relaxed);
            slot.flags.store(0, std::memory_order_relaxed);
        }
    }
}

void erase_queues_for_device_writer(VkDevice device) noexcept {
    for (auto& slot : g_queues) {
        if (slot.device.load(std::memory_order_relaxed) == device) {
            slot.queue.store(VK_NULL_HANDLE, std::memory_order_release);
            slot.device.store(VK_NULL_HANDLE, std::memory_order_relaxed);
            slot.queue_present.store(nullptr, std::memory_order_relaxed);
            slot.native_notify.store(nullptr, std::memory_order_relaxed);
        }
    }
}

void refresh_queues_for_device_writer(
    VkDevice device, PFN_vkQueuePresentKHR queue_present,
    PFN_vkQueueNotifyOutOfBandNV native_notify) noexcept {
    for (auto& slot : g_queues) {
        if (slot.device.load(std::memory_order_relaxed) == device) {
            slot.queue_present.store(queue_present, std::memory_order_relaxed);
            slot.native_notify.store(native_notify, std::memory_order_relaxed);
        }
    }
}

bool snapshot_device(VkDevice device, VulkanDeviceState* out_state, std::uint64_t generation) noexcept {
    for (const auto& slot : g_devices) {
        if (slot.device.load(std::memory_order_acquire) != device)
            continue;

        VulkanDeviceState state{};
        state.device = device;
        state.create_semaphore = slot.create_semaphore.load(std::memory_order_relaxed);
        state.destroy_semaphore = slot.destroy_semaphore.load(std::memory_order_relaxed);
        state.signal_semaphore = slot.signal_semaphore.load(std::memory_order_relaxed);
        state.anti_lag_update = slot.anti_lag_update.load(std::memory_order_relaxed);
        state.get_device_queue = slot.get_device_queue.load(std::memory_order_relaxed);
        state.get_device_queue2 = slot.get_device_queue2.load(std::memory_order_relaxed);
        state.acquire_next_image = slot.acquire_next_image.load(std::memory_order_relaxed);
        state.acquire_next_image2 = slot.acquire_next_image2.load(std::memory_order_relaxed);
        state.create_swapchain = slot.create_swapchain.load(std::memory_order_relaxed);
        state.queue_present = slot.queue_present.load(std::memory_order_relaxed);
        state.destroy_swapchain = slot.destroy_swapchain.load(std::memory_order_relaxed);
        state.create_graphics_pipelines = slot.create_graphics_pipelines.load(std::memory_order_relaxed);
        state.create_compute_pipelines = slot.create_compute_pipelines.load(std::memory_order_relaxed);
        state.wait_for_present = slot.wait_for_present.load(std::memory_order_relaxed);
        state.wait_for_present2 = slot.wait_for_present2.load(std::memory_order_relaxed);
        state.get_past_presentation_timing = slot.get_past_presentation_timing.load(std::memory_order_relaxed);
        state.nv_set_latency_sleep_mode = slot.nv_set_latency_sleep_mode.load(std::memory_order_relaxed);
        state.nv_latency_sleep = slot.nv_latency_sleep.load(std::memory_order_relaxed);
        state.nv_set_latency_marker = slot.nv_set_latency_marker.load(std::memory_order_relaxed);
        state.nv_get_latency_timings = slot.nv_get_latency_timings.load(std::memory_order_relaxed);
        state.nv_queue_notify_out_of_band = slot.nv_queue_notify_out_of_band.load(std::memory_order_relaxed);
        state.low_latency_semaphore = slot.low_latency_semaphore.load(std::memory_order_relaxed);
        const auto flags = slot.flags.load(std::memory_order_relaxed);
        state.amd_anti_lag_enabled = (flags & kFlagAmdAntiLag) != 0;
        state.nv_low_latency2_enabled = (flags & kFlagNvLowLatency2) != 0;
        state.nv_low_latency2_native = (flags & kFlagNvLowLatency2Native) != 0;
        state.present_id_enabled = (flags & kFlagPresentId) != 0;
        state.present_id2_enabled = (flags & kFlagPresentId2) != 0;
        state.present_wait_enabled = (flags & kFlagPresentWait) != 0;
        state.present_wait2_enabled = (flags & kFlagPresentWait2) != 0;
        state.present_timing_enabled = (flags & kFlagPresentTiming) != 0;
        state.capabilities.api_version = slot.capability_api_version.load(std::memory_order_relaxed);
        state.capabilities.driver_version = slot.capability_driver_version.load(std::memory_order_relaxed);
        state.capabilities.vendor_id = slot.capability_vendor_id.load(std::memory_order_relaxed);
        state.capabilities.device_id = slot.capability_device_id.load(std::memory_order_relaxed);
        state.capabilities.supported = slot.capability_supported.load(std::memory_order_relaxed);
        state.capabilities.enabled = slot.capability_enabled.load(std::memory_order_relaxed);
        state.capabilities.enabled_known = slot.capability_enabled_known.load(std::memory_order_relaxed);
        state.capabilities.eds3_supported = slot.capability_eds3_supported.load(std::memory_order_relaxed);
        state.capabilities.eds3_enabled = slot.capability_eds3_enabled.load(std::memory_order_relaxed);
        state.capabilities.eds3_enabled_known = slot.capability_eds3_enabled_known.load(std::memory_order_relaxed);
        state.capabilities.support_probe_complete = (flags & kFlagCapabilityProbeComplete) != 0;

        // Reject a torn publication and retry without ever entering a mutex.
        if (g_generation.load(std::memory_order_acquire) != generation ||
            slot.device.load(std::memory_order_acquire) != device) {
            return false;
        }

        *out_state = state;
        return true;
    }

    return g_generation.load(std::memory_order_acquire) == generation;
}

const VulkanDeviceState* cached_device_state(VkDevice device) noexcept {
    if (device == VK_NULL_HANDLE)
        return nullptr;

    for (;;) {
        const auto generation = stable_generation();
        if (g_tls_device.valid &&
            g_tls_device.device == device &&
            g_tls_device.generation == generation) {
            return g_tls_device.state.device == device ? &g_tls_device.state : nullptr;
        }

        VulkanDeviceState state{};
        if (!snapshot_device(device, &state, generation))
            continue;

        g_tls_device.device = device;
        g_tls_device.generation = generation;
        g_tls_device.valid = true;
        g_tls_device.state = state;
        return state.device == device ? &g_tls_device.state : nullptr;
    }
}

bool snapshot_queue(VkQueue queue, ThreadLocalQueueCache* out, std::uint64_t generation) noexcept {
    for (const auto& slot : g_queues) {
        if (slot.queue.load(std::memory_order_acquire) != queue)
            continue;

        const auto device = slot.device.load(std::memory_order_relaxed);
        const auto queue_present = slot.queue_present.load(std::memory_order_relaxed);
        const auto native_notify = slot.native_notify.load(std::memory_order_relaxed);
        if (g_generation.load(std::memory_order_acquire) != generation ||
            slot.queue.load(std::memory_order_acquire) != queue) {
            return false;
        }

        out->queue = queue;
        out->device = device;
        out->queue_present = queue_present;
        out->native_notify = native_notify;
        out->generation = generation;
        out->valid = true;
        out->fallback_resolved = true;
        return true;
    }

    if (g_generation.load(std::memory_order_acquire) != generation)
        return false;

    out->queue = queue;
    out->device = VK_NULL_HANDLE;
    out->queue_present = nullptr;
    out->native_notify = nullptr;
    out->generation = generation;
    out->valid = true;
    out->fallback_resolved = false;
    return true;
}

ThreadLocalQueueCache* cached_queue_state(VkQueue queue) noexcept {
    if (queue == VK_NULL_HANDLE)
        return nullptr;

    for (;;) {
        const auto generation = stable_generation();
        if (g_tls_queue.valid &&
            g_tls_queue.queue == queue &&
            g_tls_queue.generation == generation) {
            return &g_tls_queue;
        }

        if (snapshot_queue(queue, &g_tls_queue, generation))
            return &g_tls_queue;
    }
}

} // namespace

bool VulkanDeviceRegistry::register_device(const VulkanDeviceState& state) {
    if (state.device == VK_NULL_HANDLE)
        return false;

    std::scoped_lock lock(g_writer_mutex);
    auto* slot = find_device_slot_writer(state.device);
    if (!slot)
        slot = find_free_device_slot_writer();

    if (!slot) {
        spdlog::error("Vulkan device registry is full ({} devices)", kMaxTrackedVulkanDevices);
        return false;
    }

    MutationScope mutation;
    publish_device_slot(*slot, state);
    refresh_queues_for_device_writer(
        state.device,
        state.queue_present,
        state.nv_low_latency2_native ? state.nv_queue_notify_out_of_band : nullptr);
    return true;
}

void VulkanDeviceRegistry::unregister_device(VkDevice device) {
    if (device == VK_NULL_HANDLE)
        return;

    PFN_vkDestroySemaphore destroy_semaphore = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;

    {
        std::scoped_lock lock(g_writer_mutex);
        auto* slot = find_device_slot_writer(device);
        if (!slot)
            return;

        MutationScope mutation;
        destroy_semaphore = slot->destroy_semaphore.load(std::memory_order_relaxed);
        semaphore = slot->low_latency_semaphore.load(std::memory_order_relaxed);
        clear_device_slot(*slot);
        erase_queues_for_device_writer(device);
        erase_swapchains_for_device_writer(device);
    }

    if (semaphore != VK_NULL_HANDLE && destroy_semaphore)
        destroy_semaphore(device, semaphore, nullptr);
}

bool VulkanDeviceRegistry::get_state(VkDevice device, VulkanDeviceState* out_state) {
    if (!out_state)
        return false;

    const auto* state = cached_device_state(device);
    if (!state)
        return false;

    *out_state = *state;
    return true;
}

PFN_vkAcquireNextImageKHR VulkanDeviceRegistry::get_acquire_next_image(VkDevice device) {
    const auto* state = cached_device_state(device);
    return state ? state->acquire_next_image : nullptr;
}

PFN_vkAcquireNextImage2KHR VulkanDeviceRegistry::get_acquire_next_image2(VkDevice device) {
    const auto* state = cached_device_state(device);
    return state ? state->acquire_next_image2 : nullptr;
}

bool VulkanDeviceRegistry::get_queue_present_dispatch(
    VkQueue queue, VkDevice* out_device, PFN_vkQueuePresentKHR* out_present) {
    if (!out_device || !out_present)
        return false;

    const auto* state = cached_queue_state(queue);
    if (!state || state->device == VK_NULL_HANDLE)
        return false;

    *out_device = state->device;
    *out_present = state->queue_present;
    return true;
}

bool VulkanDeviceRegistry::get_nv_low_latency2_dispatch(
    VkDevice device,
    VulkanNvLowLatency2Dispatch* out_dispatch) {
    if (!out_dispatch)
        return false;

    const auto* state = cached_device_state(device);
    if (!state || !state->nv_low_latency2_enabled)
        return false;

    out_dispatch->set_sleep_mode = state->nv_set_latency_sleep_mode;
    out_dispatch->latency_sleep = state->nv_latency_sleep;
    out_dispatch->set_marker = state->nv_set_latency_marker;
    out_dispatch->get_timings = state->nv_get_latency_timings;
    out_dispatch->enabled = true;
    out_dispatch->native = state->nv_low_latency2_native;
    return true;
}

bool VulkanDeviceRegistry::get_present_precision_dispatch(
    VkDevice device,
    VulkanPresentPrecisionDispatch* out_dispatch) {
    if (!out_dispatch)
        return false;

    const auto* state = cached_device_state(device);
    if (!state)
        return false;

    out_dispatch->wait_for_present = state->wait_for_present;
    out_dispatch->wait_for_present2 = state->wait_for_present2;
    out_dispatch->get_past_presentation_timing = state->get_past_presentation_timing;
    out_dispatch->present_id_enabled = state->present_id_enabled;
    out_dispatch->present_id2_enabled = state->present_id2_enabled;
    out_dispatch->present_wait_enabled = state->present_wait_enabled;
    out_dispatch->present_wait2_enabled = state->present_wait2_enabled;
    out_dispatch->present_timing_enabled = state->present_timing_enabled;
    return true;
}

bool VulkanDeviceRegistry::get_capabilities(
    VkDevice device,
    VulkanCapabilitySnapshot* out_capabilities) {
    if (!out_capabilities)
        return false;

    const auto* state = cached_device_state(device);
    if (!state)
        return false;

    *out_capabilities = state->capabilities;
    return true;
}

bool VulkanDeviceRegistry::has_amd_anti_lag(VkDevice device) {
    const auto* state = cached_device_state(device);
    return state && state->amd_anti_lag_enabled && state->anti_lag_update;
}

bool VulkanDeviceRegistry::has_native_nv_low_latency2(VkDevice device) {
    const auto* state = cached_device_state(device);
    return state && state->nv_low_latency2_native;
}

PFN_vkAntiLagUpdateAMD VulkanDeviceRegistry::get_anti_lag_update(VkDevice device) {
    const auto* state = cached_device_state(device);
    if (!state || !state->amd_anti_lag_enabled)
        return nullptr;
    return state->anti_lag_update;
}

bool VulkanDeviceRegistry::register_queue(VkDevice device, VkQueue queue) {
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE)
        return false;

    std::scoped_lock lock(g_writer_mutex);
    auto* device_slot = find_device_slot_writer(device);
    if (!device_slot)
        return false;

    auto* slot = find_queue_slot_writer(queue);
    if (!slot)
        slot = find_free_queue_slot_writer();

    if (!slot) {
        spdlog::warn("Vulkan queue registry is full ({} queues)", kMaxTrackedVulkanQueues);
        return false;
    }

    const auto flags = device_slot->flags.load(std::memory_order_relaxed);
    const auto native_notify = (flags & kFlagNvLowLatency2Native) != 0
        ? device_slot->nv_queue_notify_out_of_band.load(std::memory_order_relaxed)
        : nullptr;
    const auto queue_present = device_slot->queue_present.load(std::memory_order_relaxed);

    MutationScope mutation;
    slot->queue.store(VK_NULL_HANDLE, std::memory_order_release);
    slot->device.store(device, std::memory_order_relaxed);
    slot->queue_present.store(queue_present, std::memory_order_relaxed);
    slot->native_notify.store(native_notify, std::memory_order_relaxed);
    slot->queue.store(queue, std::memory_order_release);
    return true;
}

bool VulkanDeviceRegistry::get_device_for_queue(VkQueue queue, VkDevice* out_device) {
    if (!out_device)
        return false;

    const auto* state = cached_queue_state(queue);
    if (!state || state->device == VK_NULL_HANDLE)
        return false;

    *out_device = state->device;
    return true;
}

bool VulkanDeviceRegistry::register_swapchain(
    VkDevice device, VkSwapchainKHR swapchain, VkSwapchainCreateFlagsKHR flags) {
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE)
        return false;

    std::scoped_lock lock(g_writer_mutex);
    auto* slot = find_swapchain_slot_writer(swapchain);
    if (!slot)
        slot = find_free_swapchain_slot_writer();
    if (!slot) {
        spdlog::warn("Vulkan swapchain registry is full ({} swapchains)", kMaxTrackedVulkanSwapchains);
        return false;
    }

    MutationScope mutation;
    slot->swapchain.store(VK_NULL_HANDLE, std::memory_order_release);
    slot->device.store(device, std::memory_order_relaxed);
    slot->flags.store(flags, std::memory_order_relaxed);
    slot->swapchain.store(swapchain, std::memory_order_release);
    return true;
}

void VulkanDeviceRegistry::unregister_swapchain(VkSwapchainKHR swapchain) {
    if (swapchain == VK_NULL_HANDLE)
        return;
    std::scoped_lock lock(g_writer_mutex);
    auto* slot = find_swapchain_slot_writer(swapchain);
    if (!slot)
        return;
    MutationScope mutation;
    slot->swapchain.store(VK_NULL_HANDLE, std::memory_order_release);
    slot->device.store(VK_NULL_HANDLE, std::memory_order_relaxed);
    slot->flags.store(0, std::memory_order_relaxed);
}

bool VulkanDeviceRegistry::get_swapchain_state(
    VkSwapchainKHR swapchain, VulkanSwapchainState* out_state) {
    if (!out_state || swapchain == VK_NULL_HANDLE)
        return false;

    for (;;) {
        const auto generation = stable_generation();
        for (const auto& slot : g_swapchains) {
            if (slot.swapchain.load(std::memory_order_acquire) != swapchain)
                continue;
            VulkanSwapchainState state{};
            state.device = slot.device.load(std::memory_order_relaxed);
            state.flags = slot.flags.load(std::memory_order_relaxed);
            if (g_generation.load(std::memory_order_acquire) != generation ||
                slot.swapchain.load(std::memory_order_acquire) != swapchain)
                break;
            *out_state = state;
            return true;
        }
        if (g_generation.load(std::memory_order_acquire) == generation)
            return false;
    }
}

PFN_vkQueueNotifyOutOfBandNV VulkanDeviceRegistry::get_native_queue_notify(VkQueue queue) {
    auto* queue_state = cached_queue_state(queue);
    if (!queue_state)
        return nullptr;
    if (queue_state->fallback_resolved)
        return queue_state->native_notify;

    // Compatibility fallback for queues obtained through an unhooked core
    // entry point. Resolve it once per TLS generation, then cache even a null
    // result so an unknown queue does not scan all device slots every frame.
    for (;;) {
        const auto generation = stable_generation();
        PFN_vkQueueNotifyOutOfBandNV candidate = nullptr;
        bool ambiguous = false;
        for (const auto& slot : g_devices) {
            if (slot.device.load(std::memory_order_acquire) == VK_NULL_HANDLE)
                continue;
            const auto flags = slot.flags.load(std::memory_order_relaxed);
            if ((flags & kFlagNvLowLatency2Native) == 0)
                continue;
            const auto notify = slot.nv_queue_notify_out_of_band.load(std::memory_order_relaxed);
            if (!notify)
                continue;
            if (candidate && candidate != notify) {
                ambiguous = true;
                break;
            }
            candidate = notify;
        }
        if (g_generation.load(std::memory_order_acquire) != generation)
            continue;

        // cached_queue_state() used the same generation or will be invalidated
        // automatically by the next registry mutation.
        queue_state->generation = generation;
        queue_state->native_notify = ambiguous ? nullptr : candidate;
        queue_state->fallback_resolved = true;
        return queue_state->native_notify;
    }
}

VkResult VulkanDeviceRegistry::ensure_low_latency_semaphore(VkDevice device, VkSemaphore* out_semaphore) {
    if (device == VK_NULL_HANDLE || !out_semaphore)
        return VK_ERROR_INITIALIZATION_FAILED;

    const auto* cached = cached_device_state(device);
    if (!cached || !cached->create_semaphore)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (cached->low_latency_semaphore != VK_NULL_HANDLE) {
        *out_semaphore = cached->low_latency_semaphore;
        return VK_SUCCESS;
    }

    VkSemaphoreTypeCreateInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_info.initialValue = 0;

    VkSemaphoreCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create_info.pNext = &timeline_info;

    VkSemaphore new_semaphore = VK_NULL_HANDLE;
    const VkResult result = cached->create_semaphore(device, &create_info, nullptr, &new_semaphore);
    if (result != VK_SUCCESS)
        return result;

    PFN_vkDestroySemaphore destroy_extra = nullptr;
    bool installed = false;

    {
        std::scoped_lock lock(g_writer_mutex);
        auto* slot = find_device_slot_writer(device);
        if (!slot) {
            destroy_extra = cached->destroy_semaphore;
        } else {
            const auto existing = slot->low_latency_semaphore.load(std::memory_order_relaxed);
            if (existing == VK_NULL_HANDLE) {
                MutationScope mutation;
                slot->low_latency_semaphore.store(new_semaphore, std::memory_order_release);
                *out_semaphore = new_semaphore;
                installed = true;
            } else {
                *out_semaphore = existing;
                destroy_extra = slot->destroy_semaphore.load(std::memory_order_relaxed);
            }
        }
    }

    if (!installed && new_semaphore != VK_NULL_HANDLE && destroy_extra)
        destroy_extra(device, new_semaphore, nullptr);

    return installed || *out_semaphore != VK_NULL_HANDLE ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult VulkanDeviceRegistry::signal_semaphore(VkDevice device, VkSemaphore semaphore, uint64_t value) {
    if (device == VK_NULL_HANDLE || semaphore == VK_NULL_HANDLE)
        return VK_ERROR_INITIALIZATION_FAILED;

    const auto* state = cached_device_state(device);
    if (!state || !state->signal_semaphore)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkSemaphoreSignalInfo signal_info{};
    signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal_info.semaphore = semaphore;
    signal_info.value = value;
    return state->signal_semaphore(device, &signal_info);
}

VkResult VulkanDeviceRegistry::signal_low_latency_semaphore(VkDevice device, uint64_t value) {
    const auto* state = cached_device_state(device);
    if (!state || state->low_latency_semaphore == VK_NULL_HANDLE || !state->signal_semaphore)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkSemaphoreSignalInfo signal_info{};
    signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal_info.semaphore = state->low_latency_semaphore;
    signal_info.value = value;
    return state->signal_semaphore(device, &signal_info);
}

void VulkanDeviceRegistry::destroy_low_latency_semaphore(VkDevice device) {
    if (device == VK_NULL_HANDLE)
        return;

    PFN_vkDestroySemaphore destroy_semaphore = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;

    {
        std::scoped_lock lock(g_writer_mutex);
        auto* slot = find_device_slot_writer(device);
        if (!slot)
            return;

        semaphore = slot->low_latency_semaphore.load(std::memory_order_relaxed);
        if (semaphore == VK_NULL_HANDLE)
            return;

        destroy_semaphore = slot->destroy_semaphore.load(std::memory_order_relaxed);
        MutationScope mutation;
        slot->low_latency_semaphore.store(VK_NULL_HANDLE, std::memory_order_release);
    }

    if (destroy_semaphore)
        destroy_semaphore(device, semaphore, nullptr);
}
