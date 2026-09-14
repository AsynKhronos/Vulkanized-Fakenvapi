#include "clock_math.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

int main() {
    using hostclock::signed_delta_ns;
    using hostclock::ticks_to_ns;

    assert(ticks_to_ns(10'000'000ull, 10'000'000ull) == 1'000'000'000ull);
    assert(ticks_to_ns(15'000'000ull, 10'000'000ull) == 1'500'000'000ull);
    assert(ticks_to_ns(3'579'545ull, 3'579'545ull) == 1'000'000'000ull);
    assert(ticks_to_ns(123, 0) == 0);

    assert(signed_delta_ns(110, 100) == 10);
    assert(signed_delta_ns(90, 100) == -10);
    assert(signed_delta_ns(100, 100) == 0);
    assert(signed_delta_ns(
        std::numeric_limits<std::uint64_t>::max(), 0) ==
        std::numeric_limits<std::int64_t>::max());

    std::cout << "canonical clock math test: PASS\n";
    return 0;
}
