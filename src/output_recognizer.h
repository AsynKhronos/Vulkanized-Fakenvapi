#pragma once

#include "runtime_policy.h"

#include <array>
#include <cstdint>
#include <optional>

namespace policy {

enum class OutputAvailability : std::uint8_t {
    Unknown,
    Unavailable,
    Available,
};

enum OutputEvidence : std::uint16_t {
    OutputEvidenceNone             = 0,
    OutputEvidenceSoftwareFallback = 1u << 0,
    OutputEvidenceModuleLoaded      = 1u << 1,
    OutputEvidenceLibraryPresent    = 1u << 2,
    OutputEvidenceDeviceInterface   = 1u << 3,
    OutputEvidenceVulkanInterop     = 1u << 4,
    OutputEvidenceNativeExtension   = 1u << 5,
    OutputEvidenceNativeFeature     = 1u << 6,
    OutputEvidenceEntryPoint        = 1u << 7,
};

struct BackendRecognition {
    OutputAvailability availability = OutputAvailability::Unknown;
    std::uint8_t confidence = 0;
    std::uint16_t evidence = OutputEvidenceNone;
};

class OutputRecognizer {
public:
    void reset() noexcept;
    void observe(
        Backend backend,
        OutputAvailability availability,
        std::uint8_t confidence,
        std::uint16_t evidence) noexcept;

    [[nodiscard]] BackendRecognition recognition(Backend backend) const noexcept;
    [[nodiscard]] OutputAvailability availability(Backend backend) const noexcept;
    [[nodiscard]] std::uint8_t confidence(Backend backend) const noexcept;
    [[nodiscard]] std::uint16_t evidence(Backend backend) const noexcept;

    // Unknown deliberately remains usable: recognizer misses must never remove
    // a backend that the existing initializer may still prove at runtime.
    [[nodiscard]] bool usable(Backend backend) const noexcept;

private:
    static constexpr std::size_t kBackendCount = 6;
    std::array<BackendRecognition, kBackendCount> state_{};

    [[nodiscard]] static std::optional<std::size_t> index_of(Backend backend) noexcept;
};

[[nodiscard]] constexpr const char* to_string(OutputAvailability value) noexcept {
    switch (value) {
        case OutputAvailability::Unknown: return "unknown";
        case OutputAvailability::Unavailable: return "unavailable";
        case OutputAvailability::Available: return "available";
    }
    return "unknown";
}

} // namespace policy
