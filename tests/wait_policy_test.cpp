#include "wait_policy.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using namespace waitpolicy;

    {
        const auto p = make_wait_plan(0);
        assert(p.timer_ns == 0 && p.spin_ns == 0);
    }
    {
        const auto p = make_wait_plan(200'000);
        assert(p.timer_ns == 0);
        assert(p.spin_ns == 200'000);
    }
    {
        const auto p = make_wait_plan(500'000);
        assert(p.timer_ns == 400'000);
        assert(p.spin_ns == 100'000);
    }
    {
        const auto p = make_wait_plan(1'000'000);
        assert(p.timer_ns == 875'000);
        assert(p.spin_ns == 125'000);
    }
    {
        const auto p = make_wait_plan(2'000'000);
        assert(p.timer_ns == 1'750'000);
        assert(p.spin_ns == 250'000);
    }
    {
        const auto p = make_wait_plan(16'666'667);
        assert(p.timer_ns == 16'166'667);
        assert(p.spin_ns == 500'000);
    }

    for (std::int64_t ns = 1; ns <= 20'000'000; ns += 13'337) {
        const auto p = make_wait_plan(ns);
        assert(p.timer_ns >= 0);
        assert(p.spin_ns >= 0);
        assert(p.timer_ns + p.spin_ns == ns);
        assert(p.spin_ns <= kMaxSpinTailNs || p.timer_ns == 0);
    }

    std::cout << "wait_policy_test: PASS\n";
    return 0;
}
