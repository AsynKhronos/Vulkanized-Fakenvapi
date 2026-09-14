#pragma once

// AMD's Anti-Lag 2 DX12 SDK header uses ID3D12Device in its public
// declarations but does not include d3d12.h itself. Keep this dependency
// explicit and ordered before the vendor header; the same ordering is used by
// the verified 0.1.0-r8 / 0.2.0 MinGW baseline.
#include <d3d12.h>

#include "../external/ffx_antilag2_dx12.h"
#include "../external/ffx_antilag2_dx11.h"

#include <atomic>
#include <detours.h>

class AL2Proxy {
public:
    using PFNAmdDxExtCreate11 = HRESULT(__cdecl*)(
        ID3D11Device* pDevice,
        AMD::AntiLag2DX11::IAmdDxExtInterface** ppAntiLagApi);

    static AMD::AntiLag2DX12::PFNAmdExtD3DCreateInterface o_AmdExtD3DCreateInterface;
    static PFNAmdDxExtCreate11 o_AmdDxExtCreate11;

    // Internal output-backend initialization temporarily enables pass-through
    // so it receives the real AMD interface rather than the game-facing proxy.
    static std::atomic<bool> disableAl2Kill;

    static HRESULT hkAmdExtD3DCreateInterface(IUnknown* pOuter, REFIID riid, void** ppvObject);
    static HRESULT hkAmdDxExtCreate11(
        ID3D11Device* pDevice,
        AMD::AntiLag2DX11::IAmdDxExtInterface** ppAntiLagApi);

    // Idempotent; retries are intentional because AMD driver DLLs may load late.
    static void hookAntiLag();
    [[nodiscard]] static bool isDx12Hooked() noexcept;
    [[nodiscard]] static bool isDx11Hooked() noexcept;
};
