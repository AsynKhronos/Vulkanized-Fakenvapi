#pragma once

namespace StreamlineCapability {

// Installs game-facing Streamline capability hooks when sl.interposer.dll is
// already present. Idempotent; safe to retry from NvAPI_Initialize after late
// Streamline initialization.
bool Init();
[[nodiscard]] bool IsHooked() noexcept;

} // namespace StreamlineCapability
