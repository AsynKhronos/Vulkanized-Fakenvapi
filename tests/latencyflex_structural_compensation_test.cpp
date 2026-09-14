#include "../external/latencyflex.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    lfx::LatencyFleX ctx;
    std::uint64_t latency = 0;
    std::uint64_t frame_time = 0;

    const auto target1 = ctx.GetWaitTarget(1);
    assert(target1 == 0);
    ctx.BeginFrame(1, target1, 10'000'000ull);
    ctx.EndFrame(1, 20'000'000ull, &latency, &frame_time);
    assert(latency == 10'000'000ull);

    const auto target2 = ctx.GetWaitTarget(2);
    ctx.BeginFrame(2, target2, 20'000'000ull);

    // 40 ms of the following 50 ms wall-clock gap is known external pipeline
    // compilation. Rebase that pause before publishing the real 70 ms end time.
    ctx.CompensateExternalStall(40'000'000ull, 15'000'000ull, 55'000'000ull);
    ctx.EndFrame(2, 70'000'000ull, &latency, &frame_time);

    assert(latency == 15'000'000ull);
    assert(frame_time == 15'000'000ull);

    // The projected timeline remains in the real clock domain after rebasing.
    const auto target3 = ctx.GetWaitTarget(3);
    assert(target3 >= 60'000'000ull);

    std::cout << "latencyflex_structural_compensation_test: PASS\n";
}
