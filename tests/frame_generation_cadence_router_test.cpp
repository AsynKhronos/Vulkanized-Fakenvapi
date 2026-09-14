#include "frame_generation_cadence_router.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using policy::FrameGenerationCadenceDetector;
using policy::FrameGenerationCadenceRouter;

int main() {
    using D = FrameGenerationCadenceDetector::Decision;

    FrameGenerationCadenceRouter router;
    FrameGenerationCadenceRouter::LocalState a{};
    FrameGenerationCadenceRouter::LocalState b{};
    constexpr std::uintptr_t A = 0x1000;
    constexpr std::uintptr_t B = 0x2000;

    bool enabled = false;
    const std::uint64_t fg_stream[] = {1,1,2,2,3,3,4,4,5,5};
    bool saw_enable = false;
    for (auto id : fg_stream) {
        const auto d = router.observe(A, id, enabled, a);
        if (d == D::Enable) {
            enabled = true;
            saw_enable = true;
        }
    }
    assert(saw_enable && enabled);
    assert(router.owner() == A);

    // A sporadic foreign callback cannot steal a healthy stream.
    assert(router.observe(B, 6, enabled, b) == D::NoChange);
    assert(router.owner() == A);
    assert(router.observe(A, 6, enabled, a) == D::NoChange);
    assert(router.owner() == A);

    // If A disappears, B may take over only after bounded render-id lag. The
    // new TLS detector starts clean but inherits the global enabled state and
    // can therefore prove a later disable instead of leaving FG stuck on.
    assert(router.observe(B, 7, enabled, b) == D::NoChange);
    assert(router.owner() == A);
    assert(router.observe(B, 8, enabled, b) == D::NoChange);
    assert(router.owner() == A);
    assert(router.observe(B, 9, enabled, b) == D::NoChange);
    assert(router.owner() == B);

    bool saw_disable = false;
    for (std::uint64_t id = 10; id < 40; ++id) {
        const auto d = router.observe(B, id, enabled, b);
        if (d == D::Disable) {
            enabled = false;
            saw_disable = true;
            break;
        }
    }
    assert(saw_disable && !enabled);

    // A large backwards jump is an epoch restart and permits immediate owner
    // migration without importing the previous producer's cadence history.
    router.reset();
    enabled = false;
    for (std::uint64_t id = 100; id <= 105; ++id)
        (void)router.observe(A, id, enabled, a);
    assert(router.owner() == A);
    assert(router.observe(B, 1, enabled, b) == D::NoChange);
    assert(router.owner() == B);

    // Reset withdraws global ownership. TLS intentionally has no per-present
    // generation load; a producer that repeats the exact last id therefore
    // fails open until its next bounded progress-publication point. It cannot
    // mutate global FG state while owner()==0.
    router.reset();
    assert(router.owner() == 0);
    (void)router.observe(B, 1, false, b);
    assert(router.owner() == 0);
    (void)router.observe(B, 2, false, b);
    assert(router.owner() == 0);
    (void)router.observe(B, 3, false, b);
    assert(router.owner() == B);

    std::cout << "PASS: lock-free FG cadence ownership and bounded migration\n";
}
