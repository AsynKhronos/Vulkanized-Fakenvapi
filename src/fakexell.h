#pragma once

namespace fakexell {

// Installs the XeLL export detours when libxell.dll is present.  The function
// is idempotent so control-plane callers may retry after a late DLL load.
bool Init();
[[nodiscard]] bool IsHooked() noexcept;

} // namespace fakexell
