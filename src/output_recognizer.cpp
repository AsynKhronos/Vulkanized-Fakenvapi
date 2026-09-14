#include "output_recognizer.h"

#include <algorithm>

namespace policy {

std::optional<std::size_t> OutputRecognizer::index_of(Backend backend) noexcept {
    switch (backend) {
        case Backend::AntiLag2: return 0;
        case Backend::XeLL: return 1;
        case Backend::AmdAntiLagVk: return 2;
        case Backend::LatencyFlex: return 3;
        case Backend::NativeReflex: return 4;
        case Backend::VulkanFlex: return 5;
        case Backend::Auto: return std::nullopt;
    }
    return std::nullopt;
}

void OutputRecognizer::reset() noexcept {
    state_ = {};
}

void OutputRecognizer::observe(
    Backend backend,
    OutputAvailability availability_value,
    std::uint8_t confidence_value,
    std::uint16_t evidence_value) noexcept {
    const auto index = index_of(backend);
    if (!index.has_value()) return;

    auto& state = state_[*index];
    state.evidence = static_cast<std::uint16_t>(state.evidence | evidence_value);
    state.confidence = std::max(state.confidence, confidence_value);

    // Positive proof wins over earlier uncertainty/negative probing. Negative
    // proof wins over Unknown but can still be superseded by a later direct
    // successful probe (for example after a delayed driver/module load).
    if (availability_value == OutputAvailability::Available ||
        (availability_value == OutputAvailability::Unavailable &&
         state.availability == OutputAvailability::Unknown)) {
        state.availability = availability_value;
    }
}

BackendRecognition OutputRecognizer::recognition(Backend backend) const noexcept {
    const auto index = index_of(backend);
    return index.has_value() ? state_[*index] : BackendRecognition{};
}

OutputAvailability OutputRecognizer::availability(Backend backend) const noexcept {
    return recognition(backend).availability;
}

std::uint8_t OutputRecognizer::confidence(Backend backend) const noexcept {
    return recognition(backend).confidence;
}

std::uint16_t OutputRecognizer::evidence(Backend backend) const noexcept {
    return recognition(backend).evidence;
}

bool OutputRecognizer::usable(Backend backend) const noexcept {
    return availability(backend) != OutputAvailability::Unavailable;
}

} // namespace policy
