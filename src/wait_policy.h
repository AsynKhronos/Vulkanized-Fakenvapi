#pragma once

#include <algorithm>
#include <cstdint>

namespace waitpolicy {

struct WaitPlan {
    std::int64_t timer_ns = 0;
    std::int64_t spin_ns = 0;
};

// Keep only a short precision tail on the calling thread. The old 2 ms fixed
// busy-wait could burn a substantial fraction of one CPU core at high refresh
// rates and steal SMT resources from the game. The tail scales with the wait so
// short sleeps remain precise while long sleeps spend almost all time blocked.
inline constexpr std::int64_t kMinSpinTailNs = 100'000;   // 0.10 ms
inline constexpr std::int64_t kMaxSpinTailNs = 500'000;   // 0.50 ms
inline constexpr std::int64_t kMinTimerSliceNs = 250'000; // avoid a syscall for tiny coarse slices

[[nodiscard]] constexpr WaitPlan make_wait_plan(std::int64_t total_ns) noexcept {
    if (total_ns <= 0)
        return {};

    auto spin = std::clamp<std::int64_t>(
        total_ns / 8, kMinSpinTailNs, kMaxSpinTailNs);
    spin = std::min(spin, total_ns);

    auto timer = total_ns - spin;
    if (timer < kMinTimerSliceNs) {
        // Very short waits are cheaper and more deterministic as one bounded
        // spin than as a kernel transition followed by an equally small tail.
        return WaitPlan{0, total_ns};
    }

    return WaitPlan{timer, spin};
}

} // namespace waitpolicy
