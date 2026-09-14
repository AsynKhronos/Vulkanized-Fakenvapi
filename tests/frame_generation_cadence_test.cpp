#include "frame_generation_cadence.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using policy::FrameGenerationCadenceDetector;

int main() {
    using D = FrameGenerationCadenceDetector::Decision;

    FrameGenerationCadenceDetector detector;
    bool enabled = false;

    // Ordinary one-present-per-render cadence must not trigger FG.
    for (std::uint64_t id = 1; id <= 20; ++id)
        assert(detector.observe(id, enabled) == D::NoChange);
    assert(detector.repeat_count() == 0);

    detector.reset();
    // Five duplicate render ids inside the rolling comparison window are
    // enough to prove a generated-presentation cadence.
    const std::uint64_t stream[] = {1,1,2,2,3,3,4,4,5,5};
    bool saw_enable = false;
    for (auto id : stream) {
        const auto d = detector.observe(id, enabled);
        if (d == D::Enable) {
            enabled = true;
            saw_enable = true;
        }
    }
    assert(saw_enable);
    assert(enabled);

    // Hysteresis: a few unique presentations do not immediately disable FG.
    for (std::uint64_t id = 6; id <= 14; ++id)
        assert(detector.observe(id, enabled) != D::Disable);

    bool saw_disable = false;
    for (std::uint64_t id = 15; id <= 30; ++id) {
        const auto d = detector.observe(id, enabled);
        if (d == D::Disable) {
            enabled = false;
            saw_disable = true;
            break;
        }
    }
    assert(saw_disable);
    assert(!enabled);

    // Backwards IDs are treated as an epoch boundary; old duplicate evidence
    // cannot leak into the restarted stream.
    detector.reset();
    enabled = false;
    for (auto id : stream) {
        const auto d = detector.observe(id, enabled);
        if (d == D::Enable) enabled = true;
    }
    assert(enabled);
    assert(detector.observe(1, enabled) == D::NoChange);
    assert(detector.repeat_count() == 0);

    std::cout << "PASS: O(1) frame-generation cadence detector\n";
}
