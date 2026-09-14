#include "completion_feedback_policy.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using completionfeedback::Candidate;
    using completionfeedback::observe;

    Candidate c{};
    observe(c, 10, 1'000, 2, 2, false);
    assert(c.valid && !c.precise && c.frame_id == 10 && c.completion_count == 1);

    // Newer fallback replaces older fallback.
    observe(c, 12, 1'200, 2, 2, false);
    assert(!c.precise && c.frame_id == 12 && c.timestamp_ns == 1'200);
    assert(c.completion_count == 2);

    // Precise evidence wins even when it refers to an older frame: quality is
    // more valuable than a CPU-present fallback timestamp for estimator input.
    observe(c, 11, 0, 2, 2, true);
    assert(c.precise && c.frame_id == 11);
    assert(c.completion_count == 3);

    // Within precise evidence, newest frame wins.
    observe(c, 15, 0, 2, 2, true);
    assert(c.precise && c.frame_id == 15);
    assert(c.completion_count == 4);

    // A newer fallback cannot displace a precise sample, but it still counts as
    // a real retired completion for queue-depth accounting.
    observe(c, 18, 1'800, 2, 2, false);
    assert(c.precise && c.frame_id == 15);
    assert(c.completion_count == 5);

    // Stale epochs are ignored completely.
    observe(c, 99, 9'900, 1, 2, true);
    assert(c.frame_id == 15 && c.completion_count == 5);

    std::cout << "completion_feedback_policy_test: PASS\n";
    return 0;
}
