#pragma once

#include "low_latency_tech.h"

#include <atomic>
#include <vulkan/vulkan_core.h>

// D3D12 -> vkd3d-proton -> Vulkan direct execution path.
//
// Unlike AntiLag2, this backend does not call AMD's Windows D3D extension ABI
// on each frame. It obtains the Vulkan device from vkd3d-proton's public
// ID3D12DXVKInteropDevice interface once during initialization, resolves
// vkAntiLagUpdateAMD once, and emits the two VK_AMD_anti_lag stage updates
// directly from the frozen Hybrid marker timeline.
class Vkd3dAntiLagVk final : public LowLatencyTech {
private:
    HMODULE vulkan_module_ = nullptr;
    VkDevice vk_device_ = VK_NULL_HANDLE;
    PFN_vkAntiLagUpdateAMD anti_lag_update_ = nullptr;
    std::atomic<std::uint64_t> last_input_frame_{INVALID_ID};
    std::atomic<std::uint64_t> last_present_frame_{INVALID_ID};
    std::atomic<std::uint64_t> paired_input_frame_{INVALID_ID};
    std::atomic<bool> pairing_active_{false};
    std::atomic<bool> requested_enabled_{false};
    std::atomic<std::uint32_t> max_fps_{0};
    std::atomic<ForceReflex> override_mode_{ForceReflex::InGame};
    std::atomic<std::uint64_t> hot_control_{0};
    static constexpr std::uint64_t kHotEnabled = 1ull << 32;

    void rebuild_hot_control() noexcept;
    void emit_input(std::uint64_t frame_id) noexcept;
    void emit_stage(VkAntiLagStageAMD stage, std::uint64_t frame_id) noexcept;

public:
    Vkd3dAntiLagVk() = default;

    bool init(IUnknown* pDevice) override;
    bool init_using_ctx(void* context) override;
    void deinit() override;

    Mode get_mode() override { return Mode::AntiLagVk; }
    void* get_tech_context() override { return nullptr; }
    void set_fg_type(bool, std::uint64_t) override {}
    void set_low_latency_override(ForceReflex value) override;
    void set_effective_fg_state(bool) override {}

    bool is_enabled() override {
        return (hot_control_.load(std::memory_order_acquire) & kHotEnabled) != 0;
    }

    void get_sleep_status(SleepParams* sleep_params) override;
    void set_sleep_mode(SleepMode* sleep_mode) override;
    void sleep() override {}
    void sleep_with_frame_id(std::uint64_t frame_id) override;
    void set_marker(IUnknown* pDevice, MarkerParams* marker_params) override;
    void set_async_marker(MarkerParams* marker_params) override;
};
