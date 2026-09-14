#pragma once

#include <cstddef>
#include <cstdint>

namespace policy {

// O(1) detector for presentation streams where generated presentations reuse
// the source/render frame id. It intentionally observes cadence only; ownership
// and canonical render/present identity stay in HybridFusion.
class FrameGenerationCadenceDetector {
public:
    enum class Decision : std::uint8_t {
        NoChange,
        Enable,
        Disable,
    };

    static constexpr std::size_t kComparisonWindow = 11;
    static constexpr std::uint8_t kEnableRepeatThreshold = 5;

    void reset() noexcept {
        previous_frame_id_ = 0;
        repeat_bits_ = 0;
        comparisons_ = 0;
        repeat_count_ = 0;
        initialized_ = false;
    }

    [[nodiscard]] Decision observe(std::uint64_t frame_id, bool currently_enabled) noexcept {
        if (!initialized_) {
            previous_frame_id_ = frame_id;
            initialized_ = true;
            return Decision::NoChange;
        }

        // A backwards id normally means a producer/session epoch changed. Old
        // cadence evidence must not leak into the new sequence.
        if (frame_id < previous_frame_id_) {
            previous_frame_id_ = frame_id;
            repeat_bits_ = 0;
            comparisons_ = 0;
            repeat_count_ = 0;
            return Decision::NoChange;
        }

        const std::uint8_t repeated = frame_id == previous_frame_id_ ? 1u : 0u;
        previous_frame_id_ = frame_id;

        // 11 comparison results fit in a single register. Shift in the newest
        // duplicate bit and subtract the bit that falls out of the rolling
        // window. This replaces the former byte array + cursor with branch-light
        // fixed-state arithmetic while preserving the exact detector semantics.
        constexpr std::uint16_t kWindowMask =
            static_cast<std::uint16_t>((1u << kComparisonWindow) - 1u);
        const std::uint8_t evicted = comparisons_ == kComparisonWindow
            ? static_cast<std::uint8_t>((repeat_bits_ >> (kComparisonWindow - 1)) & 1u)
            : 0u;
        repeat_bits_ = static_cast<std::uint16_t>(((repeat_bits_ << 1) | repeated) & kWindowMask);
        if (comparisons_ < kComparisonWindow)
            ++comparisons_;
        repeat_count_ = static_cast<std::uint8_t>(repeat_count_ + repeated - evicted);

        if (!currently_enabled && repeat_count_ >= kEnableRepeatThreshold)
            return Decision::Enable;

        // Hysteresis: do not disable from one short run of unique presents.
        // Require an entire comparison window with no duplicate render ids.
        if (currently_enabled && comparisons_ == kComparisonWindow && repeat_count_ == 0)
            return Decision::Disable;

        return Decision::NoChange;
    }

    [[nodiscard]] std::uint8_t repeat_count() const noexcept { return repeat_count_; }
    [[nodiscard]] std::uint8_t comparisons() const noexcept { return comparisons_; }

private:
    std::uint64_t previous_frame_id_ = 0;
    std::uint16_t repeat_bits_ = 0;
    std::uint8_t comparisons_ = 0;
    std::uint8_t repeat_count_ = 0;
    bool initialized_ = false;
};

} // namespace policy
