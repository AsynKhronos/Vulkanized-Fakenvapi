#include "low_latency.h"

namespace {

policy::NormalizedMarker normalize_marker(MarkerType marker) noexcept {
    using policy::NormalizedMarker;
    switch (marker) {
        case MarkerType::SIMULATION_START: return NormalizedMarker::SimulationStart;
        case MarkerType::SIMULATION_END: return NormalizedMarker::SimulationEnd;
        case MarkerType::RENDERSUBMIT_START: return NormalizedMarker::RenderSubmitStart;
        case MarkerType::RENDERSUBMIT_END: return NormalizedMarker::RenderSubmitEnd;
        case MarkerType::PRESENT_START: return NormalizedMarker::PresentStart;
        case MarkerType::PRESENT_END: return NormalizedMarker::PresentEnd;
        case MarkerType::INPUT_SAMPLE: return NormalizedMarker::InputSample;
        case MarkerType::TRIGGER_FLASH: return NormalizedMarker::TriggerFlash;
        case MarkerType::PC_LATENCY_PING: return NormalizedMarker::PcLatencyPing;
        case MarkerType::OUT_OF_BAND_RENDERSUBMIT_START: return NormalizedMarker::OutOfBandRenderSubmitStart;
        case MarkerType::OUT_OF_BAND_RENDERSUBMIT_END: return NormalizedMarker::OutOfBandRenderSubmitEnd;
        case MarkerType::OUT_OF_BAND_PRESENT_START: return NormalizedMarker::OutOfBandPresentStart;
        case MarkerType::OUT_OF_BAND_PRESENT_END: return NormalizedMarker::OutOfBandPresentEnd;
    }
    return NormalizedMarker::SimulationStart;
}

std::optional<std::size_t> frontend_index(policy::InputFrontend frontend) noexcept {
    switch (frontend) {
        case policy::InputFrontend::Reflex: return 0;
        case policy::InputFrontend::VkNvLowLatency2: return 1;
        case policy::InputFrontend::XeLL: return 2;
        case policy::InputFrontend::AntiLag2: return 3;
        case policy::InputFrontend::Auto: return std::nullopt;
    }
    return std::nullopt;
}

constexpr std::uint64_t kSleepModeValid = 1ull << 0;
constexpr std::uint64_t kSleepModeEnabled = 1ull << 1;
constexpr std::uint64_t kSleepModeBoost = 1ull << 2;
constexpr std::uint64_t kSleepModeMarkers = 1ull << 3;

std::uint64_t pack_sleep_mode(const SleepMode& mode) noexcept {
    std::uint64_t packed = kSleepModeValid;
    if (mode.low_latency_enabled) packed |= kSleepModeEnabled;
    if (mode.low_latency_boost) packed |= kSleepModeBoost;
    if (mode.use_markers_to_optimize) packed |= kSleepModeMarkers;
    packed |= static_cast<std::uint64_t>(mode.minimum_interval_us) << 32;
    return packed;
}

SleepMode unpack_sleep_mode(std::uint64_t packed) noexcept {
    SleepMode mode{};
    mode.low_latency_enabled = (packed & kSleepModeEnabled) != 0;
    mode.low_latency_boost = (packed & kSleepModeBoost) != 0;
    mode.use_markers_to_optimize = (packed & kSleepModeMarkers) != 0;
    mode.minimum_interval_us = static_cast<std::uint32_t>(packed >> 32);
    return mode;
}


} // namespace

// private
bool LowLatency::observe_input(policy::InputFrontend frontend, MarkerType marker, uint64_t frame_id) {
    // Once Hybrid startup ownership is frozen, the selected pacing source is
    // immutable for the session. Do not keep paying quality/freshness telemetry
    // atomics on every frame after that one-time decision.
    if (hybrid_hotpath_locked_.load(std::memory_order_acquire)) {
        return selected_input.load(std::memory_order_relaxed) == frontend;
    }

    input_arbiter.observe(frontend, normalize_marker(marker), frame_id);

    // Frontend callbacks can arrive concurrently (for example Reflex through
    // Streamline while the Anti-Lag 2 proxy reports the same frame). Selection
    // therefore has to be a single atomic state transition during startup.
    const auto& snapshot = Config::get().snapshot();

    for (;;) {
        auto previous = selected_input.load(std::memory_order_acquire);
        std::optional<policy::InputFrontend> next;
        if (hybrid_execution_active()) {
            // Startup-locked Hybrid collects evidence first. There is no
            // provisional pacing owner: until the one-time startup decision is
            // frozen, no frontend is allowed to drive the execution backend.
            next = hybrid_fusion_.resolve_pacing_source(
                snapshot, input_arbiter, frontend_selection_mask());
        } else {
            next = input_arbiter.select(snapshot, previous, frontend_selection_mask());
        }
        const auto desired = next.value_or(policy::InputFrontend::Auto);

        if (desired == previous) {
            if (desired != policy::InputFrontend::Auto && hybrid_fusion_.locked()) {
                hybrid_hotpath_locked_.store(true, std::memory_order_release);
            }
            return desired == frontend;
        }

        if (!selected_input.compare_exchange_weak(
                previous,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // Another frontend committed a selection while this callback was
            // evaluating.  Re-evaluate against that new incumbent instead of
            // overwriting it from a stale snapshot.
            continue;
        }

        applied_sleep_mode_.store(0, std::memory_order_release);
        applied_sleep_frontend_.store(policy::InputFrontend::Auto, std::memory_order_release);
        applied_hybrid_signature_.store(0, std::memory_order_release);
        frontend_control_epoch_.fetch_add(1, std::memory_order_release);
        if (desired != policy::InputFrontend::Auto && hybrid_fusion_.locked()) {
            // Publish the frozen fast-path flag only after selected_input has
            // been committed, so concurrent callbacks can never observe a
            // locked session with an uncommitted pacing owner.
            hybrid_hotpath_locked_.store(true, std::memory_order_release);
        }

        if (desired != policy::InputFrontend::Auto) {
            if (hybrid_execution_active()) {
                spdlog::info("Locked low-latency pacing source for session: {} (quality {})",
                             policy::to_string(desired), input_arbiter.quality(desired));

                const auto reflex = input_arbiter.recognition(policy::InputFrontend::Reflex);
                const auto vkll2 = input_arbiter.recognition(policy::InputFrontend::VkNvLowLatency2);
                const auto xell = input_arbiter.recognition(policy::InputFrontend::XeLL);
                const auto al2 = input_arbiter.recognition(policy::InputFrontend::AntiLag2);
                spdlog::info(
                    "Input detector frozen: reflex=q{}/e0x{:02x}/m0x{:04x}, vkll2=q{}/e0x{:02x}/m0x{:04x}, xell=q{}/e0x{:02x}/m0x{:04x}, al2=q{}/e0x{:02x}/m0x{:04x}",
                    reflex.quality, reflex.evidence_mask, reflex.marker_mask,
                    vkll2.quality, vkll2.evidence_mask, vkll2.marker_mask,
                    xell.quality, xell.evidence_mask, xell.marker_mask,
                    al2.quality, al2.evidence_mask, al2.marker_mask);
            } else {
                spdlog::info("Selected low-latency input: {} (quality {})",
                             policy::to_string(desired), input_arbiter.quality(desired));
            }
        } else {
            spdlog::info("No low-latency input currently meets the configured quality threshold");
        }

        return desired == frontend;
    }
}

bool LowLatency::input_can_drive(policy::InputFrontend frontend) const noexcept {
    return selected_input.load(std::memory_order_acquire) == frontend;
}

bool LowLatency::frontend_requested_enabled(
    policy::InputFrontend frontend, bool* enabled) const noexcept {
    if (!enabled) return false;

    const auto index = frontend_index(frontend);
    if (!index.has_value()) return false;

    const std::uint64_t packed =
        frontend_control_state_[*index].sleep_mode.load(std::memory_order_acquire);
    if ((packed & kSleepModeValid) == 0) return false;

    *enabled = (packed & kSleepModeEnabled) != 0;
    return true;
}

bool LowLatency::frontend_selection_enabled(policy::InputFrontend frontend) const noexcept {
    const auto index = frontend_index(frontend);
    if (!index.has_value()) return false;

    const std::uint64_t packed =
        frontend_control_state_[*index].sleep_mode.load(std::memory_order_acquire);

    // Some integrations begin emitting markers before publishing an explicit
    // sleep-mode packet.  Keep those sources eligible until they tell us their
    // state.  Once a frontend explicitly reports OFF, continue observing its
    // markers for quality/freshness telemetry but do not let it win arbitration.
    if ((packed & kSleepModeValid) == 0) return true;

    if (frontend == policy::InputFrontend::Reflex) {
        const auto forced = Config::get().get_force_reflex();
        if (forced == ForceReflex::ForceDisable) return false;
        if (forced == ForceReflex::ForceEnable) return true;
    }

    return (packed & kSleepModeEnabled) != 0;
}

std::uint8_t LowLatency::frontend_selection_mask() const noexcept {
    std::uint8_t mask = 0;
    constexpr policy::InputFrontend frontends[] = {
        policy::InputFrontend::Reflex,
        policy::InputFrontend::VkNvLowLatency2,
        policy::InputFrontend::XeLL,
        policy::InputFrontend::AntiLag2,
    };

    for (std::size_t i = 0; i < 4; ++i) {
        if (frontend_selection_enabled(frontends[i])) {
            mask |= static_cast<std::uint8_t>(1u << i);
        }
    }
    return mask;
}

bool LowLatency::observe_sleep_input(policy::InputFrontend frontend, uint64_t frame_id, uint64_t* effective_frame_id) {
    const auto index = frontend_index(frontend);
    if (!index.has_value()) return false;

    if (frame_id == INVALID_ID) {
        frame_id = frontend_sleep_sequence_[*index].fetch_add(1, std::memory_order_acq_rel) + 1;
    } else {
        frontend_sleep_sequence_[*index].store(frame_id, std::memory_order_release);
    }
    if (effective_frame_id) *effective_frame_id = frame_id;

    if (hybrid_hotpath_locked_.load(std::memory_order_acquire)) {
        // The pacing source's native frame ID is translated into the internal
        // monotonic FrameToken. XeLL and every fused marker receive this same
        // execution ID even when their original frontend IDs differ.
        const auto token = hybrid_fusion_.canonicalize_pacing_observation(frontend, frame_id);
        if (token.has_value()) {
            if (effective_frame_id) *effective_frame_id = token->sequence;
            if (auto* observer = transport_observer_published_.load(std::memory_order_acquire))
                observer->observe_canonical_frame_start(token->sequence);
        }
        return selected_input.load(std::memory_order_relaxed) == frontend;
    }

    input_arbiter.note_evidence(frontend, policy::InputEvidenceSleep);

    if (hybrid_execution_active()) {
        // Before startup lock this only contributes pacing evidence. After the
        // one-time lock the second call below materializes the first FrameToken.
        hybrid_fusion_.publish_pacing_observation(frontend, frame_id);
    }

    // Sleep is a strong once-per-frame cadence signal and semantically occurs
    // at the simulation/input boundary for all currently supported frontends.
    // It contributes to selection quality without dispatching a second marker
    // to the output backend.
    const bool drives = observe_input(frontend, MarkerType::SIMULATION_START, frame_id);
    if (drives && hybrid_fusion_.locked()) {
        const auto token = hybrid_fusion_.canonicalize_pacing_observation(frontend, frame_id);
        if (token.has_value()) {
            if (effective_frame_id) *effective_frame_id = token->sequence;
            if (auto* observer = transport_observer_published_.load(std::memory_order_acquire))
                observer->observe_canonical_frame_start(token->sequence);
        }
    }
    return drives;
}

std::uint8_t LowLatency::hybrid_control_fields(policy::InputFrontend frontend) noexcept {
    using namespace policy;
    switch (frontend) {
        case InputFrontend::Reflex:
        case InputFrontend::VkNvLowLatency2:
            return HybridControlEnabled | HybridControlBoost |
                   HybridControlMinimumInterval | HybridControlMarkers;
        case InputFrontend::XeLL:
            return HybridControlEnabled | HybridControlBoost | HybridControlMinimumInterval;
        case InputFrontend::AntiLag2:
            // Anti-Lag 2 v1 carries enable/maxFPS, but has no Reflex/XeLL-style
            // boost bit and no use-markers-to-optimize control field.
            return HybridControlEnabled | HybridControlMinimumInterval;
        case InputFrontend::Auto:
            return 0;
    }
    return 0;
}

bool LowLatency::hybrid_execution_active() const noexcept {
    const auto& snapshot = Config::get().snapshot();
    if (!snapshot.hybrid.startup_locked)
        return false;

    const auto backend = active_backend_published_.load(std::memory_order_acquire);
    const auto api = active_api_published_.load(std::memory_order_acquire);
    if (api == policy::GraphicsApi::D3D12) {
        if (backend == policy::Backend::VulkanFlex)
            return snapshot.hybrid.vulkan_fusion && snapshot.vulkanflex.vkd3d_bridge;
        return snapshot.hybrid.xell_fusion &&
               (backend == policy::Backend::XeLL || backend == policy::Backend::AmdAntiLagVk);
    }
    if (api == policy::GraphicsApi::D3D11) {
        return snapshot.hybrid.vulkan_fusion && snapshot.vulkanflex.dxvk_bridge &&
               backend == policy::Backend::VulkanFlex;
    }
    if (api == policy::GraphicsApi::Vulkan) {
        return snapshot.hybrid.vulkan_fusion && backend == policy::Backend::VulkanFlex;
    }
    return false;
}

void LowLatency::FrontendObserveEvidence(
    policy::InputFrontend frontend, policy::InputEvidence evidence) noexcept {
    input_arbiter.note_evidence(frontend, evidence);
}

void LowLatency::store_frontend_sleep_mode(policy::InputFrontend frontend, const SleepMode& mode) noexcept {
    if (!hybrid_hotpath_locked_.load(std::memory_order_acquire)) {
        input_arbiter.note_evidence(frontend, policy::InputEvidenceControl);
    }

    const auto index = frontend_index(frontend);
    if (!index.has_value()) return;

    const auto packed = pack_sleep_mode(mode);
    const auto previous = frontend_control_state_[*index].sleep_mode.exchange(
        packed, std::memory_order_acq_rel);

    policy::HybridControlPacket packet{};
    packet.enabled = mode.low_latency_enabled;
    packet.boost = mode.low_latency_boost;
    packet.minimum_interval_us = mode.minimum_interval_us;
    packet.use_markers_to_optimize = mode.use_markers_to_optimize;
    packet.valid_fields = hybrid_control_fields(frontend);
    hybrid_fusion_.publish_control(frontend, packet);

    if (previous != packed) {
        frontend_control_epoch_.fetch_add(1, std::memory_order_release);
    }
}

void LowLatency::apply_frontend_sleep_mode(policy::InputFrontend frontend) {
    const auto index = frontend_index(frontend);
    if (!index.has_value()) return;

    const bool hybrid_locked = hybrid_hotpath_locked_.load(std::memory_order_acquire);
    const auto control_epoch = frontend_control_epoch_.load(std::memory_order_acquire);
    if (hybrid_locked &&
        applied_control_epoch_.load(std::memory_order_acquire) == control_epoch) {
        return;
    }

    if (hybrid_locked || hybrid_execution_active()) {
        const auto& snapshot = Config::get().snapshot();
        const auto resolved = hybrid_fusion_.resolve_control(
            snapshot, input_arbiter, frontend_selection_mask());
        if (!resolved.has_value()) return;

        const std::uint64_t prior_signature =
            applied_hybrid_signature_.load(std::memory_order_acquire);
        if (prior_signature == resolved->signature) {
            applied_control_epoch_.store(control_epoch, std::memory_order_release);
            return;
        }

        const bool first_lock_application = prior_signature == 0;

        SleepMode mode{};
        mode.low_latency_enabled = resolved->value.enabled;
        mode.low_latency_boost = resolved->value.boost;
        mode.minimum_interval_us = resolved->value.minimum_interval_us;
        mode.use_markers_to_optimize = resolved->value.use_markers_to_optimize;

        {
            // Control changes are rare. Serialize only the actual backend
            // mutation; the unchanged per-frame path returned above without a
            // mutex, allocation, scorer or virtual call.
            std::scoped_lock lock(active_tech_mutex);
            if (!currently_active_tech) return;
            if (applied_hybrid_signature_.load(std::memory_order_relaxed) == resolved->signature) {
                applied_control_epoch_.store(control_epoch, std::memory_order_release);
                return;
            }
            currently_active_tech->set_sleep_mode(&mode);
            applied_sleep_mode_.store(pack_sleep_mode(mode), std::memory_order_release);
            applied_sleep_frontend_.store(resolved->pacing_source, std::memory_order_release);
            applied_hybrid_signature_.store(resolved->signature, std::memory_order_release);
            applied_control_epoch_.store(control_epoch, std::memory_order_release);
        }

        if (first_lock_application) {
            spdlog::info(
                "Hybrid startup lock: pacing={}, enabled={}({}), boost={}({}), interval_us={}({}), markers={}({})",
                policy::to_string(resolved->pacing_source),
                resolved->value.enabled, policy::to_string(resolved->enabled_source),
                resolved->value.boost, policy::to_string(resolved->boost_source),
                resolved->value.minimum_interval_us, policy::to_string(resolved->interval_source),
                resolved->value.use_markers_to_optimize, policy::to_string(resolved->markers_source));
        } else {
            spdlog::info(
                "Hybrid locked control update: pacing={}, enabled={}({}), boost={}({}), interval_us={}({}), markers={}({})",
                policy::to_string(resolved->pacing_source),
                resolved->value.enabled, policy::to_string(resolved->enabled_source),
                resolved->value.boost, policy::to_string(resolved->boost_source),
                resolved->value.minimum_interval_us, policy::to_string(resolved->interval_source),
                resolved->value.use_markers_to_optimize, policy::to_string(resolved->markers_source));
        }

        if (first_lock_application) {
            const auto source_name = [&](policy::HybridAspect aspect) {
                return policy::to_string(
                    hybrid_fusion_.source_for_aspect(aspect).value_or(policy::InputFrontend::Auto));
            };
            spdlog::info(
                "Hybrid frozen aspects: simulation={}, render={}, present={}, input={}, oob={}, fg={}",
                source_name(policy::HybridAspect::SimulationLifecycle),
                source_name(policy::HybridAspect::RenderLifecycle),
                source_name(policy::HybridAspect::PresentLifecycle),
                source_name(policy::HybridAspect::InputSampling),
                source_name(policy::HybridAspect::OutOfBandLifecycle),
                source_name(policy::HybridAspect::FrameGeneration));
        }
        return;
    }

    const std::uint64_t packed = frontend_control_state_[*index].sleep_mode.load(std::memory_order_acquire);
    if ((packed & kSleepModeValid) == 0) return;

    if (applied_sleep_frontend_.load(std::memory_order_acquire) == frontend &&
        applied_sleep_mode_.load(std::memory_order_acquire) == packed) {
        return;
    }

    SleepMode mode = unpack_sleep_mode(packed);
    std::scoped_lock lock(active_tech_mutex);
    if (!currently_active_tech) return;
    if (applied_sleep_frontend_.load(std::memory_order_relaxed) == frontend &&
        applied_sleep_mode_.load(std::memory_order_relaxed) == packed) {
        return;
    }
    currently_active_tech->set_sleep_mode(&mode);
    applied_sleep_mode_.store(packed, std::memory_order_release);
    applied_sleep_frontend_.store(frontend, std::memory_order_release);
    applied_hybrid_signature_.store(0, std::memory_order_release);
    applied_control_epoch_.store(control_epoch, std::memory_order_release);
}

bool LowLatency::hybrid_accept_marker(
    policy::InputFrontend frontend, MarkerType marker, uint64_t frame_id,
    uint64_t* canonical_frame_id) noexcept {
    if (hybrid_hotpath_locked_.load(std::memory_order_acquire)) {
        const auto token = hybrid_fusion_.canonicalize_marker_locked(
            frontend, normalize_marker(marker), frame_id);
        if (!token.has_value()) return false;
        if (canonical_frame_id) *canonical_frame_id = token->sequence;
        return true;
    }
    if (!hybrid_execution_active()) {
        if (canonical_frame_id) *canonical_frame_id = frame_id;
        return input_can_drive(frontend);
    }

    const auto token = hybrid_fusion_.canonicalize_marker(
        frontend,
        normalize_marker(marker),
        frame_id,
        Config::get().snapshot(),
        input_arbiter,
        frontend_selection_mask());
    if (hybrid_fusion_.locked()) {
        hybrid_hotpath_locked_.store(true, std::memory_order_release);
    }
    if (!token.has_value()) return false;
    if (canonical_frame_id) *canonical_frame_id = token->sequence;
    return true;
}

void LowLatency::update_effective_fg_state() {
    const std::int8_t forced = forced_fg_state_.load(std::memory_order_acquire);
    const bool desired = forced >= 0 ? forced != 0 : fg_.load(std::memory_order_acquire);
    const std::uint8_t encoded = desired ? 1u : 0u;

    if (applied_fg_state_.load(std::memory_order_acquire) == encoded) return;

    std::scoped_lock lock(active_tech_mutex);
    if (!currently_active_tech) return;
    if (applied_fg_state_.load(std::memory_order_relaxed) == encoded) return;

    currently_active_tech->set_effective_fg_state(desired);
    applied_fg_state_.store(encoded, std::memory_order_release);
}

void LowLatency::update_enabled_override() {
    const auto desired = Config::get().get_force_reflex();
    const auto encoded = static_cast<std::uint8_t>(desired);
    if (applied_override_.load(std::memory_order_acquire) == encoded) return;

    std::scoped_lock lock(active_tech_mutex);
    if (!currently_active_tech) return;
    if (applied_override_.load(std::memory_order_relaxed) == encoded) return;

    currently_active_tech->set_low_latency_override(desired);
    applied_override_.store(encoded, std::memory_order_release);
}

// R4.1 XRFlex observer plane -------------------------------------------------
void LowLatency::OpenXROnWaitFrame(
    std::uintptr_t session,
    std::int64_t predicted_display_time,
    std::int64_t predicted_display_period,
    bool should_render,
    std::uint64_t return_timestamp_ns,
    std::optional<std::uint64_t> predicted_display_host_ns) noexcept {
    // openxr_hooks.cpp already gates this observer entry from one config
    // snapshot. Avoid a second config publication load in every XR frame.
    if (session == 0) return;

    const auto latest_render = hybrid_fusion_.latest_canonical_frame();
    const auto render_watermark = latest_render ? latest_render->sequence : 0;
    const auto render_epoch_watermark = latest_render ? latest_render->epoch : 0;
    const auto token = xr_timeline_.on_wait(
        session,
        predicted_display_time,
        predicted_display_period,
        should_render,
        return_timestamp_ns,
        render_watermark,
        predicted_display_host_ns,
        render_epoch_watermark);
    if (!token) return;

    bool expected = false;
    if (xr_timeline_logged_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        spdlog::info(
            "XRFlex OpenXR timeline attached: timing_owner=openxr-runtime, observer_only=true, "
            "session=0x{:x}, predicted_period_ns={}, should_render={}",
            session,
            predicted_display_period,
            should_render);
    }

    if (token->predicted_clock_valid) {
        bool clock_expected = false;
        if (xr_clock_logged_.compare_exchange_strong(
                clock_expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            spdlog::info(
                "XRFlex canonical clock active: source=XR_KHR_win32_convert_performance_counter_time, "
                "host_clock=qpc, xr_frame={}, wait_to_display_us={}",
                token->sequence, token->wait_to_display_ns / 1000);
        }
    }
}

void LowLatency::OpenXROnBeginFrame(
    std::uintptr_t session,
    bool discarded_previous,
    std::uint64_t return_timestamp_ns) noexcept {
    if (session == 0) return;
    (void)xr_timeline_.on_begin(session, discarded_previous, return_timestamp_ns);
}

void LowLatency::OpenXROnEndFrame(
    std::uintptr_t session,
    std::int64_t display_time,
    std::uint64_t return_timestamp_ns,
    std::optional<std::uint64_t> submitted_display_host_ns) noexcept {
    if (session == 0) return;

    const auto token = xr_timeline_.on_end(
        session,
        display_time,
        return_timestamp_ns,
        hybrid_fusion_.latest_canonical_frame(),
        submitted_display_host_ns);
    if (!token || token->render_sequence == 0) return;

    bool expected = false;
    if (xr_correlation_logged_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        spdlog::info(
            "XRFlex graphics correlation active: xr_frame={}, render_frame={}, "
            "submitted_display_time={}, end_to_display_us={}, clock_valid={}, timing_owner=openxr-runtime",
            token->sequence,
            token->render_sequence,
            token->submitted_display_time,
            token->end_to_display_ns / 1000,
            token->submitted_clock_valid);
    }
}

void LowLatency::OpenXROnDestroySession(std::uintptr_t session) noexcept {
    if (session == 0) return;
    xr_timeline_.on_destroy_session(session);
}

// public
bool LowLatency::deinit_current_tech() {
    std::scoped_lock lock(active_tech_mutex);

    deinit_translation_observer();

    if (currently_active_tech) {
        // Withdraw the published route before destroying the backend so fast
        // paths cannot treat a backend undergoing teardown as stable.
        active_backend_published_.store(policy::Backend::Auto, std::memory_order_release);
        active_tech_published_.store(nullptr, std::memory_order_release);
        active_d3d_device_published_.store(nullptr, std::memory_order_release);
        active_vk_device_published_.store(VK_NULL_HANDLE, std::memory_order_release);
        applied_routing_signature_published_.store(0, std::memory_order_release);

        currently_active_tech->deinit();

        delete currently_active_tech;
        currently_active_tech = nullptr;

        std::memset(frame_reports, 0, sizeof(frame_reports));
        active_backend = policy::Backend::Auto;
        active_d3d_device = nullptr;
        active_vk_device = VK_NULL_HANDLE;
        applied_routing_signature = 0;
        pending_routing_signature = 0;
        applied_sleep_mode_.store(0, std::memory_order_release);
        applied_sleep_frontend_.store(policy::InputFrontend::Auto, std::memory_order_release);
        applied_hybrid_signature_.store(0, std::memory_order_release);
        applied_control_epoch_.store(0, std::memory_order_release);
        hybrid_hotpath_locked_.store(false, std::memory_order_release);
        applied_fg_state_.store(0xff, std::memory_order_release);
        applied_override_.store(0xff, std::memory_order_release);
        fg_.store(false, std::memory_order_release);
        fg_cadence_router_.reset();
        latency_not_ready_logged_.store(false, std::memory_order_release);
        shadow_vulkan_logged_.store(false, std::memory_order_release);
        selected_input.store(policy::InputFrontend::Auto, std::memory_order_release);
        input_arbiter.reset();
        hybrid_fusion_.reset();

        return true;
    }

    return false;
}

bool LowLatency::get_low_latency_context(void** low_latency_context, Mode* low_latency_tech) {
    std::scoped_lock lock(active_tech_mutex);

    if (!currently_active_tech || !low_latency_context || !low_latency_tech)
        return false;

    *low_latency_context = currently_active_tech->get_tech_context();
    *low_latency_tech = currently_active_tech->get_mode();

    // We are during deinit, don't let app use the context
    if (delay_deinit > 0) {
        *low_latency_context = nullptr;
        delay_deinit = 1;
    }

    return true;
}

bool LowLatency::set_low_latency_context(void* low_latency_context, Mode low_latency_tech) {
    forced_low_latency_context = low_latency_context;
    forced_low_latency_tech = low_latency_tech;

    deinit_current_tech();

    // Only D3D
    if (forced_low_latency_context)
        return update_low_latency_tech((IUnknown*) nullptr);
    else
        return true; // no device, low latency will need to reinit itself on the next frontend call
}
