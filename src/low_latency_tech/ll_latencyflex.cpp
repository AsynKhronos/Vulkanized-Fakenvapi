#include "ll_latencyflex.h"
#include "config.h"
#include "log.h"

void LatencyFlex::lfx_sleep(std::uint64_t reflex_frame_id) {
    if (!is_enabled())
        return;

    // Keep the context alive across the sleep interval, but do not hold the
    // operation mutex while sleeping: EndFrame from the render thread must be
    // able to feed the estimator while the simulation thread is parked.
    std::scoped_lock lifetime_lock(lifetime_mutex_);

    const LFXMode mode = Config::get().get_latencyflex_mode();
    const auto encoded_mode = static_cast<std::uint8_t>(mode);
    if (!previous_mode_valid_ || previous_mode_ != encoded_mode) {
        if (previous_mode_valid_)
            needs_reset_.store(true, std::memory_order_release);
        previous_mode_ = encoded_mode;
        previous_mode_valid_ = true;
    }

    if (needs_reset_.exchange(false, std::memory_order_acq_rel)) {
        spdlog::info("LFX Reset");
        eepy(200000000ULL);
        {
            std::scoped_lock lock(operation_mutex_);
            frame_id_ = 1;
            if (ctx)
                ctx->Reset();
        }
    }

    const std::uint64_t current_timestamp = get_timestamp();
    std::uint64_t timestamp = current_timestamp;

    if (mode == LFXMode::Conservative)
        lfx_end_frame(INVALID_ID); // preserve legacy conservative lifecycle

    std::uint64_t local_frame_id = reflex_frame_id;
    std::uint64_t wait_target = 0;
    {
        std::scoped_lock lock(operation_mutex_);
        if (mode != LFXMode::ReflexIDs)
            local_frame_id = frame_id_ + 1;
        if (ctx) {
            // target_frame_time is part of LatencyFleX state and is read by
            // EndFrame(), so publish it under the same external lock.
            ctx->target_frame_time = 1000ULL * minimum_interval_us_.load(std::memory_order_relaxed);
            wait_target = ctx->GetWaitTarget(local_frame_id);
        }
    }

    if (wait_target > current_timestamp) {
        const std::uint64_t timeout_timestamp = current_timestamp + 50000000ULL;
        if (wait_target > timeout_timestamp) {
            timestamp = timeout_timestamp;
            if (++timeout_events_ > 5)
                needs_reset_.store(true, std::memory_order_release);
        } else {
            timestamp = wait_target;
            timeout_events_ = 0;
        }
        if (auto res = eepy(timestamp - current_timestamp); res)
            spdlog::error("Sleep command failed: {}", res);
    } else {
        timeout_events_ = 0;
    }

    VFN_HOT_TRACE("LatencyFlex Call Spot: {}", current_call_spot == CallSpot::SimulationStart ? "SimulationStart" : "SleepCall");

    {
        std::scoped_lock lock(operation_mutex_);
        ++frame_id_;
        if (ctx)
            ctx->BeginFrame(local_frame_id, wait_target, timestamp);
    }
}

void LatencyFlex::lfx_end_frame(std::uint64_t reflex_frame_id) {
    if (!is_enabled())
        return;

    const auto current_timestamp = get_timestamp();
    const auto mode = Config::get().get_latencyflex_mode();
    std::uint64_t latency = 0;
    std::uint64_t frame_time = 1;
    std::uint64_t end_frame_id = reflex_frame_id;

    {
        std::scoped_lock lock(operation_mutex_);
        if (mode != LFXMode::ReflexIDs)
            end_frame_id = frame_id_;
        if (ctx)
            ctx->EndFrame(end_frame_id, current_timestamp, &latency, &frame_time);
    }
    VFN_HOT_TRACE("LFX latency: {}, frame_time: {}, current_timestamp: {}", latency, frame_time, current_timestamp);
}

bool LatencyFlex::init(IUnknown* pDevice) {
    std::scoped_lock lifetime_lock(lifetime_mutex_);
    if (!ctx) {
        std::scoped_lock lock(operation_mutex_);
        ctx = new lfx::LatencyFleX();
        frame_id_ = 0;
        timeout_events_ = 0;
        previous_mode_valid_ = false;
        needs_reset_.store(false, std::memory_order_relaxed);
        spdlog::info("LatencyFleX initialized");
        return true;
    }

    spdlog::error("LatencyFleX already initialized, this should not happen");
    return false;
};

bool LatencyFlex::init_using_ctx(void* context) {
    spdlog::error("LatencyFleX init_using_ctx is not supported");
    inited_using_context = false;
    return false;
}

void LatencyFlex::deinit() {
    std::scoped_lock lifetime_lock(lifetime_mutex_);
    std::scoped_lock lock(operation_mutex_);
    if (ctx) {
        delete ctx;
        ctx = nullptr;
        spdlog::info("LatencyFlex deinitialized");
    }
};

void* LatencyFlex::get_tech_context() {
    std::scoped_lock lifetime_lock(lifetime_mutex_);
    return ctx;
};

void LatencyFlex::get_sleep_status(SleepParams* sleep_params) {
    sleep_params->low_latency_enabled = is_enabled();
    sleep_params->fullscreen_vrr = true;
    sleep_params->control_panel_vsync_override = false;
};

void LatencyFlex::set_sleep_mode(SleepMode* sleep_mode) {
    const bool was_enabled = is_enabled();

    enabled_.store(sleep_mode->low_latency_enabled, std::memory_order_release);
    minimum_interval_us_.store(sleep_mode->minimum_interval_us, std::memory_order_release);

    const bool now_enabled = is_enabled();
    if (!was_enabled && now_enabled) {
        // The context may have been alive while Reflex was Off and marker
        // traffic continued. Reset before the first active BeginFrame() so
        // an in-game Off->On transition starts from a clean LFX epoch.
        needs_reset_.store(true, std::memory_order_release);
    }
};

void LatencyFlex::sleep() {
    const auto mode = Config::get().get_latencyflex_mode();
    if (mode != LFXMode::ReflexIDs) {
        last_sleep_framecount_ = simulation_framecount_;
        if (current_call_spot == CallSpot::SleepCall)
            lfx_sleep(INVALID_ID);
    }
};

void LatencyFlex::set_marker(IUnknown* pDevice, MarkerParams* marker_params) {
    switch (marker_params->marker_type) {
        case MarkerType::SIMULATION_START:
            ++simulation_framecount_;

            if (last_sleep_framecount_ + kCallSpotSwitchThreshold < simulation_framecount_)
                current_call_spot = CallSpot::SimulationStart;
            else
                current_call_spot = CallSpot::SleepCall;

            if (current_call_spot == CallSpot::SimulationStart)
                lfx_sleep(marker_params->frame_id);
            break;

        case MarkerType::RENDERSUBMIT_END:
            if (Config::get().get_latencyflex_mode() != LFXMode::Conservative)
                lfx_end_frame(marker_params->frame_id);
            break;

        default:
            break;
    }
};
