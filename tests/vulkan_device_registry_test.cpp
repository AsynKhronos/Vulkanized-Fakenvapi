#include "vulkan_device_registry.h"

#include <cassert>
#include <cstdint>

namespace {
int create_calls = 0;
int destroy_calls = 0;
int signal_calls = 0;
uint64_t last_signal_value = 0;

VkSemaphore fake_semaphore() {
    return reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(0x1234));
}

VkQueue fake_queue() {
    return reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x5678));
}

VKAPI_ATTR VkResult VKAPI_CALL fake_create_semaphore(
    VkDevice,
    const VkSemaphoreCreateInfo* create_info,
    const VkAllocationCallbacks*,
    VkSemaphore* semaphore) {

    assert(create_info);
    assert(create_info->sType == VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    assert(create_info->pNext);

    const auto* timeline = static_cast<const VkSemaphoreTypeCreateInfo*>(create_info->pNext);
    assert(timeline->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
    assert(timeline->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE);

    ++create_calls;
    *semaphore = fake_semaphore();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_semaphore(
    VkDevice,
    VkSemaphore semaphore,
    const VkAllocationCallbacks*) {
    assert(semaphore == fake_semaphore());
    ++destroy_calls;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_signal_semaphore(
    VkDevice,
    const VkSemaphoreSignalInfo* signal_info) {
    assert(signal_info);
    assert(signal_info->sType == VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO);
    assert(signal_info->semaphore == fake_semaphore());
    ++signal_calls;
    last_signal_value = signal_info->value;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_anti_lag_update(VkDevice, const VkAntiLagDataAMD*) {}

VKAPI_ATTR void VKAPI_CALL fake_queue_notify_a(
    VkQueue,
    const VkOutOfBandQueueTypeInfoNV*) {}

VKAPI_ATTR void VKAPI_CALL fake_queue_notify_b(
    VkQueue,
    const VkOutOfBandQueueTypeInfoNV*) {}

VKAPI_ATTR VkResult VKAPI_CALL fake_acquire_next_image(
    VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* image_index) {
    if (image_index) *image_index = 2;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_acquire_next_image2(
    VkDevice, const VkAcquireNextImageInfoKHR*, uint32_t* image_index) {
    if (image_index) *image_index = 3;
    return VK_SUCCESS;
}


VKAPI_ATTR VkResult VKAPI_CALL fake_create_swapchain(
    VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR* swapchain) {
    if (swapchain) *swapchain = reinterpret_cast<VkSwapchainKHR>(static_cast<uintptr_t>(0x7777));
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_wait_for_present(
    VkDevice, VkSwapchainKHR, uint64_t, uint64_t) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_wait_for_present2(
    VkDevice, VkSwapchainKHR, const VkPresentWait2InfoKHR*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_get_past_presentation_timing(
    VkDevice, const VkPastPresentationTimingInfoEXT*, VkPastPresentationTimingPropertiesEXT*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_queue_present(
    VkQueue, const VkPresentInfoKHR*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_queue_present_b(
    VkQueue, const VkPresentInfoKHR*) {
    return VK_SUBOPTIMAL_KHR;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_swapchain(
    VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) {}
} // namespace

int main() {
    const auto device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x42));

    VulkanDeviceState state{};
    state.device = device;
    state.create_semaphore = fake_create_semaphore;
    state.destroy_semaphore = fake_destroy_semaphore;
    state.signal_semaphore = fake_signal_semaphore;
    state.anti_lag_update = fake_anti_lag_update;
    state.acquire_next_image = fake_acquire_next_image;
    state.acquire_next_image2 = fake_acquire_next_image2;
    state.create_swapchain = fake_create_swapchain;
    state.queue_present = fake_queue_present;
    state.wait_for_present = fake_wait_for_present;
    state.wait_for_present2 = fake_wait_for_present2;
    state.get_past_presentation_timing = fake_get_past_presentation_timing;
    state.destroy_swapchain = fake_destroy_swapchain;
    state.amd_anti_lag_enabled = true;
    state.nv_low_latency2_enabled = true;
    state.nv_low_latency2_native = true;
    state.present_id_enabled = true;
    state.present_id2_enabled = true;
    state.present_wait_enabled = true;
    state.present_wait2_enabled = true;
    state.present_timing_enabled = true;
    state.nv_queue_notify_out_of_band = fake_queue_notify_a;
    state.capabilities.api_version = VK_API_VERSION_1_4;
    state.capabilities.vendor_id = 0x1002;
    state.capabilities.supported =
        vulkan_capability_bit(VulkanCapability::Vulkan13) |
        vulkan_capability_bit(VulkanCapability::Vulkan14) |
        vulkan_capability_bit(VulkanCapability::ExtendedDynamicState3);
    state.capabilities.enabled_known = state.capabilities.supported;
    state.capabilities.enabled = state.capabilities.supported;
    state.capabilities.eds3_supported =
        vulkan_eds3_bit(VulkanEds3Feature::PolygonMode) |
        vulkan_eds3_bit(VulkanEds3Feature::ColorBlendEnable);
    state.capabilities.eds3_enabled_known = state.capabilities.eds3_supported;
    state.capabilities.eds3_enabled = state.capabilities.eds3_supported;
    state.capabilities.support_probe_complete = true;

    assert(VulkanDeviceRegistry::register_device(state));
    assert(VulkanDeviceRegistry::has_amd_anti_lag(device));
    assert(VulkanDeviceRegistry::has_native_nv_low_latency2(device));
    assert(VulkanDeviceRegistry::get_anti_lag_update(device) == fake_anti_lag_update);

    VulkanDeviceState read_back{};
    assert(VulkanDeviceRegistry::get_state(device, &read_back));
    assert(read_back.low_latency_semaphore == VK_NULL_HANDLE);
    assert(read_back.acquire_next_image == fake_acquire_next_image);
    assert(read_back.acquire_next_image2 == fake_acquire_next_image2);
    assert(read_back.create_swapchain == fake_create_swapchain);
    assert(read_back.queue_present == fake_queue_present);
    assert(VulkanDeviceRegistry::get_acquire_next_image(device) == fake_acquire_next_image);
    assert(VulkanDeviceRegistry::get_acquire_next_image2(device) == fake_acquire_next_image2);
    assert(read_back.wait_for_present == fake_wait_for_present);
    assert(read_back.wait_for_present2 == fake_wait_for_present2);
    assert(read_back.get_past_presentation_timing == fake_get_past_presentation_timing);
    assert(read_back.destroy_swapchain == fake_destroy_swapchain);
    VulkanCapabilitySnapshot capabilities{};
    assert(VulkanDeviceRegistry::get_capabilities(device, &capabilities));
    assert(capabilities.api_version == VK_API_VERSION_1_4);
    assert(capabilities.vendor_id == 0x1002);
    assert(capabilities.support_probe_complete);
    assert(capabilities.supports(VulkanCapability::Vulkan14));
    assert(capabilities.is_enabled(VulkanCapability::ExtendedDynamicState3));
    assert(capabilities.supports_eds3(VulkanEds3Feature::PolygonMode));

    VkSemaphore semaphore = VK_NULL_HANDLE;
    assert(VulkanDeviceRegistry::ensure_low_latency_semaphore(device, &semaphore) == VK_SUCCESS);
    assert(semaphore == fake_semaphore());
    assert(create_calls == 1);

    // The second call must reuse the per-device semaphore rather than allocate.
    VkSemaphore semaphore_again = VK_NULL_HANDLE;
    assert(VulkanDeviceRegistry::ensure_low_latency_semaphore(device, &semaphore_again) == VK_SUCCESS);
    assert(semaphore_again == semaphore);
    assert(create_calls == 1);

    // The generation change caused by semaphore installation must invalidate the
    // earlier thread-local state copy.
    assert(VulkanDeviceRegistry::get_state(device, &read_back));
    assert(read_back.low_latency_semaphore == semaphore);

    assert(VulkanDeviceRegistry::signal_low_latency_semaphore(device, 77) == VK_SUCCESS);
    assert(signal_calls == 1);
    assert(last_signal_value == 77);

    assert(VulkanDeviceRegistry::signal_semaphore(device, semaphore, 88) == VK_SUCCESS);
    assert(signal_calls == 2);
    assert(last_signal_value == 88);

    assert(VulkanDeviceRegistry::register_queue(device, fake_queue()));
    VkDevice queue_device = VK_NULL_HANDLE;
    assert(VulkanDeviceRegistry::get_device_for_queue(fake_queue(), &queue_device));
    assert(queue_device == device);
    PFN_vkQueuePresentKHR queue_present = nullptr;
    VkDevice present_device = VK_NULL_HANDLE;
    assert(VulkanDeviceRegistry::get_queue_present_dispatch(
        fake_queue(), &present_device, &queue_present));
    assert(present_device == device);
    assert(queue_present == fake_queue_present);
    assert(VulkanDeviceRegistry::get_native_queue_notify(fake_queue()) == fake_queue_notify_a);

    // Re-publishing a device must invalidate TLS snapshots and refresh the
    // direct queue dispatch cache without making readers take the writer lock.
    VulkanDeviceState updated = state;
    updated.low_latency_semaphore = semaphore;
    updated.queue_present = fake_queue_present_b;
    updated.nv_queue_notify_out_of_band = fake_queue_notify_b;
    assert(VulkanDeviceRegistry::register_device(updated));
    assert(VulkanDeviceRegistry::get_native_queue_notify(fake_queue()) == fake_queue_notify_b);
    queue_present = nullptr;
    present_device = VK_NULL_HANDLE;
    assert(VulkanDeviceRegistry::get_queue_present_dispatch(
        fake_queue(), &present_device, &queue_present));
    assert(present_device == device);
    assert(queue_present == fake_queue_present_b);

    VulkanPresentPrecisionDispatch precision{};
    assert(VulkanDeviceRegistry::get_present_precision_dispatch(device, &precision));
    assert(precision.present_id_enabled);
    assert(precision.present_id2_enabled);
    assert(precision.present_wait_enabled);
    assert(precision.present_wait2_enabled);
    assert(precision.present_timing_enabled);
    assert(precision.wait_for_present == fake_wait_for_present);
    assert(precision.wait_for_present2 == fake_wait_for_present2);
    assert(precision.get_past_presentation_timing == fake_get_past_presentation_timing);

    const auto swapchain = reinterpret_cast<VkSwapchainKHR>(static_cast<uintptr_t>(0x7777));
    const auto flags = static_cast<VkSwapchainCreateFlagsKHR>(
        VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT);
    assert(VulkanDeviceRegistry::register_swapchain(device, swapchain, flags));
    VulkanSwapchainState swapchain_state{};
    assert(VulkanDeviceRegistry::get_swapchain_state(swapchain, &swapchain_state));
    assert(swapchain_state.device == device);
    assert(swapchain_state.flags == flags);

    VulkanNvLowLatency2Dispatch dispatch{};
    assert(VulkanDeviceRegistry::get_nv_low_latency2_dispatch(device, &dispatch));
    assert(dispatch.enabled);
    assert(dispatch.native);

    VulkanDeviceRegistry::destroy_low_latency_semaphore(device);
    assert(destroy_calls == 1);
    assert(VulkanDeviceRegistry::get_state(device, &read_back));
    assert(read_back.low_latency_semaphore == VK_NULL_HANDLE);

    VulkanDeviceRegistry::unregister_device(device);
    assert(!VulkanDeviceRegistry::get_state(device, &read_back));
    assert(!VulkanDeviceRegistry::get_swapchain_state(swapchain, &swapchain_state));
    assert(!VulkanDeviceRegistry::get_device_for_queue(fake_queue(), &queue_device));
    assert(destroy_calls == 1);

    return 0;
}
