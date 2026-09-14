#pragma once

#include "low_latency_tech.h"

#include "../external/latencyflex.h"

#include <atomic>

class LatencyFlex : public virtual LowLatencyTech {
private:
    lfx::LatencyFleX* ctx = nullptr;

    // LatencyFleX requires external synchronization. operation_mutex_ protects
    // every access to the context itself; lifetime_mutex_ prevents destruction
    // while a pacing call is sleeping between GetWaitTarget() and BeginFrame().
    std::mutex operation_mutex_;
    std::mutex lifetime_mutex_;

    // Control can be updated from a frontend/control callback while pacing is
    // active. Keep the tiny control surface atomic instead of racing plain
    // fields used by the frame thread.
    std::atomic<std::uint32_t> minimum_interval_us_{0};
    std::atomic<bool> enabled_{false};
    std::atomic<ForceReflex> override_{ForceReflex::InGame};
    std::atomic<bool> needs_reset_{false};

    std::uint64_t last_sleep_framecount_ = 0;
    std::uint64_t simulation_framecount_ = 0;
    static constexpr std::uint64_t kCallSpotSwitchThreshold = 20;

    // operation_mutex_ serializes access to frame_id_ with EndFrame().
    std::uint64_t frame_id_ = 0;

    // Per-instance estimator-control state. These used to be function-static,
    // accidentally coupling separate LatencyFlex instances/sessions.
    std::uint8_t previous_mode_ = 0;
    bool previous_mode_valid_ = false;
    std::uint32_t timeout_events_ = 0;

    void lfx_sleep(std::uint64_t frame_id);
    void lfx_end_frame(std::uint64_t frame_id);

public:
    LatencyFlex(): LowLatencyTech() {}

    // From LowLatencyTech
    bool init(IUnknown* pDevice) override;
    bool init_using_ctx(void* context) override;
    void deinit() override;

    Mode get_mode() override { return Mode::LatencyFlex; };
    void* get_tech_context() override;
    void set_fg_type(bool interpolated, std::uint64_t frame_id) override {}; // Not used by LFX
    void set_low_latency_override(ForceReflex value) override {
        override_.store(value, std::memory_order_release);
    };
    void set_effective_fg_state(bool effective_fg_state) override {
        this->effective_fg_state = effective_fg_state;
    };

    bool is_enabled() override {
        const auto forced = override_.load(std::memory_order_acquire);
        return forced != ForceReflex::InGame
            ? forced == ForceReflex::ForceEnable
            : enabled_.load(std::memory_order_acquire);
    };

    void get_sleep_status(SleepParams* sleep_params) override;
    void set_sleep_mode(SleepMode* sleep_mode) override;
    void sleep() override;
    void set_marker(IUnknown* pDevice, MarkerParams* marker_params) override;
    void set_async_marker(MarkerParams* marker_params) override {}; // Not used by LFX
};
