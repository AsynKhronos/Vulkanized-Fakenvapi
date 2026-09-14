#include "audio_period_policy.h"

#include <cassert>

using namespace audioflex;

int main() {
    {
        constexpr AudioPeriodRange in{
            .sample_rate = 48000,
            .requested_frames = 480,
            .default_frames = 480,
            .fundamental_frames = 48,
            .min_frames = 48,
            .max_frames = 480,
        };
        constexpr auto d = choose_period_candidate(in, 5000, 50);
        static_assert(d.valid);
        static_assert(d.change);
        static_assert(d.target_frames == 240);
        static_assert(d.candidate_frames == 240);
    }
    {
        // 5 ms @ 44.1 kHz = 220.5 frames; align upward to 224 on a 4-frame fundamental.
        constexpr AudioPeriodRange in{
            .sample_rate = 44100,
            .requested_frames = 448,
            .default_frames = 448,
            .fundamental_frames = 4,
            .min_frames = 48,
            .max_frames = 448,
        };
        constexpr auto d = choose_period_candidate(in, 5000, 50);
        static_assert(d.target_frames == 224);
        static_assert(d.candidate_frames == 224);
    }
    {
        // Never increase an application's already-lower period.
        constexpr AudioPeriodRange in{
            .sample_rate = 48000,
            .requested_frames = 96,
            .default_frames = 480,
            .fundamental_frames = 48,
            .min_frames = 48,
            .max_frames = 480,
        };
        constexpr auto d = choose_period_candidate(in, 5000, 50);
        static_assert(d.valid);
        static_assert(!d.change);
        static_assert(d.candidate_frames == 96);
    }
    {
        // Maximum reduction caps an aggressive target to one conservative step.
        constexpr AudioPeriodRange in{
            .sample_rate = 48000,
            .requested_frames = 960,
            .default_frames = 960,
            .fundamental_frames = 48,
            .min_frames = 48,
            .max_frames = 960,
        };
        constexpr auto d = choose_period_candidate(in, 1000, 25);
        static_assert(d.target_frames == 48);
        static_assert(d.candidate_frames == 720);
    }
    {
        constexpr AudioPeriodRange invalid{
            .sample_rate = 48000,
            .requested_frames = 47,
            .fundamental_frames = 48,
            .min_frames = 48,
            .max_frames = 480,
        };
        constexpr auto d = choose_period_candidate(invalid, 5000, 50);
        static_assert(!d.valid);
        static_assert(!d.change);
    }
    return 0;
}
