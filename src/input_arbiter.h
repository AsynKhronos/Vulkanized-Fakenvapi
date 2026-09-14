#pragma once

#include "runtime_policy.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace policy {

enum InputEvidence : std::uint16_t {
    InputEvidenceNone          = 0,
    InputEvidenceCapability    = 1u << 0,
    InputEvidenceContext       = 1u << 1,
    InputEvidenceControl       = 1u << 2,
    InputEvidenceSleep         = 1u << 3,
    InputEvidenceMarker        = 1u << 4,
    InputEvidenceAsyncMarker   = 1u << 5,
    InputEvidenceFrameGen      = 1u << 6,
};

struct InputRecognition {
    std::uint64_t last_frame_id = 0;
    std::uint64_t last_observation_sequence = 0;
    std::uint16_t marker_mask = 0;
    std::uint16_t evidence_mask = 0;
    std::uint8_t quality = 0;
    bool seen = false;
    bool fresh = false;
};

struct InputObservationState {
    std::atomic<std::uint64_t> last_frame_id{0};
    std::atomic<std::uint64_t> last_observation_sequence{0};
    std::atomic<std::uint16_t> marker_mask{0};
    std::atomic<std::uint16_t> evidence_mask{0};
    std::atomic<std::uint8_t> quality{0};
    std::atomic<bool> seen{false};
};

class InputArbiter {
public:
    // Frontends that have not advanced to a new native frame while other
    // frontends have advanced this many frames are considered stale. This is
    // entirely frame-activity based: no clocks, syscalls or timer reads in the
    // hot path, and marker-rich frontends cannot age sparse ones unfairly.
    static constexpr std::uint64_t kStaleObservationWindow = 16;
    static constexpr std::uint8_t kSwitchQualityMargin = 8;

    void reset() noexcept;
    void note_evidence(InputFrontend frontend, InputEvidence evidence) noexcept;
    void observe(InputFrontend frontend, NormalizedMarker marker, std::uint64_t frame_id) noexcept;

    [[nodiscard]] std::uint8_t quality(InputFrontend frontend) const noexcept;
    [[nodiscard]] std::uint64_t last_frame_id(InputFrontend frontend) const noexcept;
    [[nodiscard]] std::uint16_t observed_marker_mask(InputFrontend frontend) const noexcept;
    [[nodiscard]] std::uint16_t evidence_mask(InputFrontend frontend) const noexcept;
    [[nodiscard]] InputRecognition recognition(InputFrontend frontend) const noexcept;
    [[nodiscard]] std::uint64_t observation_sequence() const noexcept;
    [[nodiscard]] bool fresh(InputFrontend frontend) const noexcept;
    [[nodiscard]] std::optional<InputFrontend> select(
        const RuntimePolicySnapshot& snapshot,
        InputFrontend current = InputFrontend::Auto,
        std::uint8_t eligible_mask = 0x0f) const noexcept;

private:
    static constexpr std::size_t kFrontendCount = 4;
    std::array<InputObservationState, kFrontendCount> state_{};
    std::atomic<std::uint64_t> observation_sequence_{0};

    [[nodiscard]] static std::optional<std::size_t> index_of(InputFrontend frontend) noexcept;
    [[nodiscard]] static std::uint8_t priority_of(
        const RuntimePolicySnapshot& snapshot,
        InputFrontend frontend) noexcept;
};

} // namespace policy
