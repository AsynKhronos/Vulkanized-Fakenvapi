#pragma once

#include "frame_generation_cadence.h"

#include <atomic>
#include <cstdint>

namespace policy {

// Lock-free ownership layer for the asynchronous OOB-present cadence detector.
//
// The common case has one producer thread for a presentation stream. That
// thread owns a TLS detector and pays only atomic loads plus an occasional
// read-only publication store when the native render id advances. A different
// callback thread may take over only when the current owner is provably stale
// by render-id progress, or when the native id clearly restarted. Detector
// state itself never crosses threads, so takeover cannot race on rolling
// history storage.
class FrameGenerationCadenceRouter {
public:
    using Decision = FrameGenerationCadenceDetector::Decision;

    struct LocalState {
        const FrameGenerationCadenceRouter* router = nullptr;
        FrameGenerationCadenceDetector detector{};
        std::uint64_t last_published_frame = 0;
        bool last_published_valid = false;
        bool owned = false;

        void bind(const FrameGenerationCadenceRouter* value) noexcept {
            router = value;
            detector.reset();
            last_published_frame = 0;
            last_published_valid = false;
            owned = false;
        }
    };

    // Publish owner progress every two native render-id steps. A foreign
    // producer must exceed that deliberate publication lag before takeover,
    // absorbing benign callback interleaving while still recovering within a
    // few frames when a runtime migrates the callback.
    static constexpr std::uint64_t kProgressPublishStride = 2;
    static constexpr std::uint64_t kTakeoverLagFrames = 3;
    // A large backwards jump is treated as a producer/session epoch restart.
    static constexpr std::uint64_t kRestartGapFrames = 32;

    void reset() noexcept {
        owner_frame_encoded_.store(0, std::memory_order_relaxed);
        owner_.store(0, std::memory_order_release);
    }

    [[nodiscard]] Decision observe(
        std::uintptr_t producer_token,
        std::uint64_t frame_id,
        bool currently_enabled,
        LocalState& local) noexcept {
        if (producer_token == 0)
            return Decision::NoChange;

        if (local.router != this)
            local.bind(this);

        // Dominant path: once a thread owns the stream, duplicate/generated
        // presents stay entirely in TLS. Revalidate ownership only when the
        // native render id advances (where we publish progress anyway), and on
        // the rare detector transition before the caller mutates global FG
        // state. This keeps the common 1:N presentation cadence free of
        // redundant owner-cacheline reads.
        if (local.owned) {
            const bool publish_progress = !local.last_published_valid ||
                frame_id < local.last_published_frame ||
                (frame_id - local.last_published_frame) >= kProgressPublishStride;
            if (publish_progress) {
                const auto published_owner = owner_.load(std::memory_order_relaxed);
                if (published_owner != producer_token) {
                    local.detector.reset();
                    local.last_published_valid = false;
                    local.owned = false;
                    // reset() and legitimate producer migration cannot touch
                    // another thread's TLS. Fall through to the normal claim
                    // path in this same callback; it will either prove a
                    // bounded takeover/restart or fail open without importing
                    // this thread's old detector history.
                } else {
                    local.last_published_frame = frame_id;
                    local.last_published_valid = true;
                    owner_frame_encoded_.store(encode_frame(frame_id), std::memory_order_relaxed);

                    const auto decision = local.detector.observe(frame_id, currently_enabled);
                    if (decision != Decision::NoChange &&
                        owner_.load(std::memory_order_acquire) != producer_token) {
                        local.detector.reset();
                        local.last_published_valid = false;
                        local.owned = false;
                        return Decision::NoChange;
                    }
                    return decision;
                }
            } else {
                const auto decision = local.detector.observe(frame_id, currently_enabled);
                if (decision != Decision::NoChange &&
                    owner_.load(std::memory_order_acquire) != producer_token) {
                    local.detector.reset();
                    local.last_published_valid = false;
                    local.owned = false;
                    return Decision::NoChange;
                }
                return decision;
            }
        }

        auto owner = owner_.load(std::memory_order_relaxed);
        bool newly_claimed = false;
        if (owner != producer_token) {
            bool may_claim = owner == 0;
            if (!may_claim) {
                const auto encoded = owner_frame_encoded_.load(std::memory_order_relaxed);
                if (encoded != 0) {
                    const auto owner_frame = decode_frame(encoded);
                    const bool owner_lagged = frame_id > owner_frame &&
                        (frame_id - owner_frame) > kTakeoverLagFrames;
                    const bool restarted = owner_frame > frame_id &&
                        (owner_frame - frame_id) >= kRestartGapFrames;
                    may_claim = owner_lagged || restarted;
                }
            }

            if (may_claim) {
                auto expected = owner;
                if (owner_.compare_exchange_strong(
                        expected, producer_token,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    owner = producer_token;
                    newly_claimed = true;
                } else {
                    owner = expected;
                }
            }

            if (owner != producer_token)
                return Decision::NoChange;
        }

        // A local state that reaches this path has either just claimed the
        // stream or has been rebound after a reset/instance change. Detector
        // history is intentionally never transferred between producer threads.
        if (newly_claimed || !local.owned) {
            local.detector.reset();
            local.last_published_valid = false;
            local.owned = true;
        }

        local.last_published_frame = frame_id;
        local.last_published_valid = true;
        owner_frame_encoded_.store(encode_frame(frame_id), std::memory_order_relaxed);

        const auto decision = local.detector.observe(frame_id, currently_enabled);
        if (decision != Decision::NoChange &&
            owner_.load(std::memory_order_acquire) != producer_token) {
            local.detector.reset();
            local.last_published_valid = false;
            local.owned = false;
            return Decision::NoChange;
        }
        return decision;
    }

    [[nodiscard]] std::uintptr_t owner() const noexcept {
        return owner_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t owner_frame() const noexcept {
        const auto encoded = owner_frame_encoded_.load(std::memory_order_acquire);
        return encoded == 0 ? 0 : decode_frame(encoded);
    }

private:
    [[nodiscard]] static constexpr std::uint64_t encode_frame(std::uint64_t frame) noexcept {
        // 0 is reserved for "not published". UINT64_MAX is not a useful native
        // frame id for this API; saturating it keeps the representation total.
        return frame == UINT64_MAX ? UINT64_MAX : frame + 1;
    }

    [[nodiscard]] static constexpr std::uint64_t decode_frame(std::uint64_t encoded) noexcept {
        return encoded == UINT64_MAX ? UINT64_MAX : encoded - 1;
    }

    std::atomic<std::uintptr_t> owner_{0};
    std::atomic<std::uint64_t> owner_frame_encoded_{0};
};

} // namespace policy
