#pragma once

#include "runtime_policy.h"
#include "output_recognizer.h"

namespace policy {

struct BackendCandidates {
    BackendOrder order{};
};

[[nodiscard]] bool backend_supported(GraphicsApi api, Backend backend) noexcept;
[[nodiscard]] Backend requested_backend(const RuntimePolicySnapshot& snapshot, GraphicsApi api) noexcept;
[[nodiscard]] const BackendOrder& fallback_order(const RuntimePolicySnapshot& snapshot, GraphicsApi api) noexcept;
[[nodiscard]] BackendCandidates build_backend_candidates(
    const RuntimePolicySnapshot& snapshot,
    GraphicsApi api,
    const OutputRecognizer* recognizer = nullptr) noexcept;

} // namespace policy
