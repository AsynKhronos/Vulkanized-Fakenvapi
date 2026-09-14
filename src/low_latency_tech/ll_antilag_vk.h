#pragma once

#include "low_latency_tech.h"

#include <atomic>

class AntiLagVk : public virtual LowLatencyTech {
private:
    std::atomic<std::uint32_t> max_fps_{0};
    std::atomic<bool> requested_enabled_{false};
    std::atomic<ForceReflex> override_mode_{ForceReflex::InGame};
    // Lower 32 bits = maxFPS, bit 32 = effective enabled. Frame markers load
    // this once instead of touching multiple control-plane atomics.
    std::atomic<std::uint64_t> hot_control_{0};
    static constexpr std::uint64_t kHotEnabled = 1ull << 32;

    void rebuild_hot_control() noexcept;

public:
    AntiLagVk(): LowLatencyTech() {}

    // From LowLatencyTech
    bool init(IUnknown *pDevice) override;
    bool init_using_ctx(void* context) override;
    void deinit() override {}; // Not used by AntiLag VK

    Mode get_mode() override { return Mode::AntiLagVk; };
    void* get_tech_context() override;
    void set_fg_type(bool interpolated, uint64_t frame_id) override {}; // Not used by AntiLag VK
    void set_low_latency_override(ForceReflex value) override;
    void set_effective_fg_state(bool) override {};

    bool is_enabled() override {
        return (hot_control_.load(std::memory_order_acquire) & kHotEnabled) != 0;
    }

    void get_sleep_status(SleepParams* sleep_params) override;
    void set_sleep_mode(SleepMode* sleep_mode) override;
    void sleep() override;
    void set_marker(IUnknown* pDevice, MarkerParams* marker_params) override;
    void set_async_marker(MarkerParams* marker_params) override {}; // Not used by AntiLag VK
};