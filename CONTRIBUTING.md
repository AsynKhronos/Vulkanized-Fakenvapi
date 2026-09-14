# Contributing to Vulkanized-Fakenvapi

Vulkanized-Fakenvapi operates at API, timing, loader, and translation-layer boundaries. Small ownership mistakes can create double pacing, invalid frame correlation, deadlocks, or title-specific regressions. Contributions therefore need evidence, not only plausible code.

## Before opening an issue

Search existing issues first. For a runtime bug, capture at least:

- game/application and version;
- graphics API (`D3D11`, `D3D12`, native Vulkan, OpenXR where relevant);
- GPU and driver/Mesa version;
- Wine/Proton build and translation stack (`VKD3D-Proton`, `DXVK`, native Windows, etc.);
- Vulkanized-Fakenvapi commit/tag;
- relevant `fakenvapi.ini` changes;
- exact steps to reproduce;
- expected and actual behavior;
- sanitized Vulkanized-Fakenvapi log excerpts around initialization, capability detection, backend selection, and failure.

Do not attach proprietary game binaries, DRM material, access tokens, crash dumps containing secrets, or private user data.

## Development rules

Changes to latency ownership or frame timing must preserve these invariants unless the architecture documentation is explicitly revised and the change is justified:

1. exactly one active pacing/execution owner per relevant frame loop;
2. native driver/runtime ownership wins when policy says it is authoritative;
3. cooperative VKD3D/DXVK observer paths do not create a second Vulkan device/WSI owner;
4. OpenXR XRFlex remains observer-only unless a future milestone deliberately changes that contract;
5. hot paths avoid unbounded allocation, blocking global locks, repeated dynamic probing, and uncontrolled logging;
6. unrelated native frame-ID domains are not compared as though they were identical;
7. fallback paths fail open rather than destabilizing the application.

## Build and test

Run the complete host suite before submitting:

```bash
./scripts/ci-host.sh
```

For changes that affect Windows-facing code, also perform a clean MinGW x64 build:

```bash
rm -rf build-win64 subprojects/detours
meson subprojects download detours
meson setup build-win64 --cross-file build-win64.txt --buildtype release
ninja -C build-win64
```

If the change affects x86 behavior or ABI, validate `build-win32.txt` as well.

Timing/pacing changes require runtime evidence in at least one representative affected path. Source audits alone are not runtime proof.

## Pull requests

Keep pull requests narrow. Explain:

- the problem and failure mode;
- why the chosen layer owns the fix;
- ownership/concurrency implications;
- new or modified tests;
- build result;
- runtime result, when applicable;
- known limitations.

Avoid unrelated formatting churn in low-level source files.

## Performance claims

Microbenchmarks must identify the host, compiler, build flags, sample methodology, and exact measured path. Do not present CPU-path nanosecond improvements as measured end-to-end game latency unless you actually measured end-to-end latency.

## Third-party code

Do not add third-party source or binaries without preserving the applicable copyright/license text and updating `THIRD_PARTY_NOTICES.md` where necessary.

## Style

The project targets modern C++23. Prefer explicit ownership, bounded state, deterministic cleanup, and clear comments around ABI/driver/runtime constraints. Performance-sensitive changes should be measurement-driven rather than speculative.
