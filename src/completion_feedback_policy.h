#pragma once

#include <cstdint>

namespace completionfeedback {

struct Candidate {
    bool valid = false;
    bool precise = false;
    std::uint64_t frame_id = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t epoch = 0;
    std::uint32_t completion_count = 0;
};

// A single acquire-side probe can discover several already-completed presents.
// Feeding every one into a latency estimator with the same (or a few us apart)
// CPU observation time fabricates an unrealistically tiny frame interval. Keep
// the accounting for every completion, but select one representative estimator
// sample. Precise completion evidence wins over fallback evidence; within the
// same quality tier the newest frame wins.
inline void observe(
    Candidate& candidate,
    std::uint64_t frame_id,
    std::uint64_t timestamp_ns,
    std::uint64_t epoch,
    std::uint64_t active_epoch,
    bool precise) noexcept {
    if (frame_id == 0 || epoch == 0 || epoch != active_epoch)
        return;

    if (candidate.completion_count != UINT32_MAX)
        ++candidate.completion_count;

    if (!candidate.valid ||
        (precise && !candidate.precise) ||
        (precise == candidate.precise && frame_id > candidate.frame_id)) {
        candidate.valid = true;
        candidate.precise = precise;
        candidate.frame_id = frame_id;
        candidate.timestamp_ns = timestamp_ns;
        candidate.epoch = epoch;
    }
}

} // namespace completionfeedback
