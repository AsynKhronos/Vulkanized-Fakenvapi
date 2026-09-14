#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace audioabi {

using ReferenceTime = LONGLONG;

struct WaveFormatEx {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
};

enum class DataFlow : int {
    Render = 0,
    Capture = 1,
    All = 2,
};

enum class ShareMode : int {
    Shared = 0,
    Exclusive = 1,
};

inline constexpr GUID CLSID_MMDeviceEnumerator_ =
    {0xbcde0395, 0xe52f, 0x467c, {0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
inline constexpr GUID IID_IMMDeviceEnumerator_ =
    {0xa95664d2, 0x9614, 0x4f35, {0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
inline constexpr GUID IID_IAudioClient_ =
    {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
inline constexpr GUID IID_IAudioClient2_ =
    {0x726778cd, 0xf60a, 0x4eda, {0x82,0xde,0xe4,0x76,0x10,0xcd,0x78,0xaa}};
inline constexpr GUID IID_IAudioClient3_ =
    {0x7ed4ee07, 0x8e67, 0x4cd4, {0x8c,0x1a,0x2b,0x7a,0x59,0x87,0xad,0x42}};
inline constexpr GUID IID_IAudioClock_ =
    {0xcd63314f, 0x3fba, 0x4a1b, {0x81,0x2c,0xef,0x96,0x35,0x87,0x28,0xe7}};

inline bool guid_equal(const GUID& a, const GUID& b) noexcept {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

using QueryInterfaceFn = HRESULT (WINAPI*)(void*, REFIID, void**);
using ReleaseFn = ULONG (WINAPI*)(void*);
using CoCreateInstanceFn = HRESULT (WINAPI*)(REFCLSID, void*, DWORD, REFIID, void**);
using CoTaskMemFreeFn = void (WINAPI*)(void*);

using EnumAudioEndpointsFn = HRESULT (WINAPI*)(void*, DataFlow, DWORD, void**);
using GetDefaultAudioEndpointFn = HRESULT (WINAPI*)(void*, DataFlow, int, void**);
using GetDeviceFn = HRESULT (WINAPI*)(void*, LPCWSTR, void**);
using CollectionItemFn = HRESULT (WINAPI*)(void*, UINT, void**);
using DeviceActivateFn = HRESULT (WINAPI*)(void*, REFIID, DWORD, void*, void**);

using AudioInitializeFn = HRESULT (WINAPI*)(
    void*, ShareMode, DWORD, ReferenceTime, ReferenceTime, const WaveFormatEx*, const GUID*);
using AudioGetBufferSizeFn = HRESULT (WINAPI*)(void*, UINT32*);
using AudioGetStreamLatencyFn = HRESULT (WINAPI*)(void*, ReferenceTime*);
using AudioGetCurrentPaddingFn = HRESULT (WINAPI*)(void*, UINT32*);
using AudioGetDevicePeriodFn = HRESULT (WINAPI*)(void*, ReferenceTime*, ReferenceTime*);
using AudioStartStopFn = HRESULT (WINAPI*)(void*);
using AudioGetServiceFn = HRESULT (WINAPI*)(void*, REFIID, void**);

using AudioGetSharedModeEnginePeriodFn = HRESULT (WINAPI*)(
    void*, const WaveFormatEx*, UINT32*, UINT32*, UINT32*, UINT32*);
using AudioGetCurrentSharedModeEnginePeriodFn = HRESULT (WINAPI*)(void*, WaveFormatEx**, UINT32*);
using AudioInitializeSharedAudioStreamFn = HRESULT (WINAPI*)(
    void*, DWORD, UINT32, const WaveFormatEx*, const GUID*);

using AudioClockGetFrequencyFn = HRESULT (WINAPI*)(void*, UINT64*);
using AudioClockGetPositionFn = HRESULT (WINAPI*)(void*, UINT64*, UINT64*);

inline void** vtable(void* object) noexcept {
    return object ? *reinterpret_cast<void***>(object) : nullptr;
}

inline HRESULT query_interface(void* object, REFIID iid, void** out) noexcept {
    if (!object || !out) return E_POINTER;
    auto** table = vtable(object);
    if (!table) return E_NOINTERFACE;
    auto fn = reinterpret_cast<QueryInterfaceFn>(table[0]);
    return fn ? fn(object, iid, out) : E_NOINTERFACE;
}

inline void release(void* object) noexcept {
    if (!object) return;
    auto** table = vtable(object);
    if (!table) return;
    auto fn = reinterpret_cast<ReleaseFn>(table[2]);
    if (fn) (void)fn(object);
}

} // namespace audioabi
