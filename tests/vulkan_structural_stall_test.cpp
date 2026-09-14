#include "../src/vulkan_structural_stall.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    StructuralStallConfig auto_cfg{};

    StructuralStallSample warm{};
    warm.previous_completion_timestamp_ns = 100'000'000ull;
    warm.completion_timestamp_ns = 160'000'000ull;
    warm.expected_gap_ns = 8'000'000ull;
    warm.baseline_samples = 4;
    warm.pending_pipeline_duration_ns = 40'000'000ull;
    warm.pending_pipeline_end_ns = 155'000'000ull;
    const auto not_warm = classify_structural_stall(auto_cfg, warm);
    assert(!not_warm.trusted);
    assert(!not_warm.structural);

    warm.baseline_samples = 8;
    const auto structural = classify_structural_stall(auto_cfg, warm);
    assert(structural.trusted);
    assert(structural.structural);
    assert(structural.raw_gap_ns == 60'000'000ull);
    assert(structural.compensation_ns == 40'000'000ull);

    auto stale = warm;
    stale.pending_pipeline_end_ns = 40'000'000ull;
    const auto stale_result = classify_structural_stall(auto_cfg, stale);
    assert(stale_result.trusted);
    assert(!stale_result.structural);

    auto off_path = warm;
    off_path.completion_timestamp_ns = 111'000'000ull;
    off_path.pending_pipeline_end_ns = 110'000'000ull;
    const auto off_path_result = classify_structural_stall(auto_cfg, off_path);
    assert(off_path_result.trusted);
    assert(!off_path_result.structural);

    StructuralStallConfig forced = auto_cfg;
    forced.mode = StructuralStallMode::Enabled;
    warm.baseline_samples = 1;
    const auto forced_result = classify_structural_stall(forced, warm);
    assert(forced_result.trusted);
    assert(forced_result.structural);

    StructuralStallConfig disabled = auto_cfg;
    disabled.mode = StructuralStallMode::Disabled;
    assert(!classify_structural_stall(disabled, warm).trusted);

    std::uint64_t baseline = 0;
    baseline = update_structural_baseline_ns(baseline, 8'000'000ull);
    assert(baseline == 8'000'000ull);
    baseline = update_structural_baseline_ns(baseline, 16'000'000ull);
    assert(baseline == 9'000'000ull);
    baseline = update_structural_baseline_ns(baseline, 1'000'000'000ull);
    assert(baseline == 9'000'000ull);

    // A plausible but isolated 60 ms hitch must not drag an 8 ms classifier
    // baseline toward the hitch. It is winsorized to 16 ms, then EWMA'd.
    std::uint64_t robust = 8'000'000ull;
    robust = update_structural_baseline_ns(robust, 60'000'000ull);
    assert(robust == 9'000'000ull);

    // Real timing changes still converge progressively in both directions.
    robust = update_structural_baseline_ns(robust, 33'000'000ull);
    assert(robust == 10'125'000ull);
    robust = update_structural_baseline_ns(robust, 4'000'000ull);
    assert(robust == 9'492'188ull);

    assert(structural_stall_kind_name(StructuralStallKind::GraphicsPipeline));
    assert(structural_stall_kind_name(StructuralStallKind::ComputePipeline));
    std::cout << "vulkan_structural_stall_test: PASS\n";
}
