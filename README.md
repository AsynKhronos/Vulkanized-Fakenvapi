# Vulkanized-Fakenvapi

[![CI](https://github.com/AsynKhronos/Vulkanized-Fakenvapi/actions/workflows/ci.yml/badge.svg)](https://github.com/AsynKhronos/Vulkanized-Fakenvapi/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C.svg)](meson.build)
![Status](https://img.shields.io/badge/status-development%20candidate-orange.svg)

**A Vulkan-first, cross-vendor low-latency compatibility layer for Windows games on Windows and Wine/Proton graphics stacks.**

Vulkanized-Fakenvapi continues the archived [OptiScaler/fakenvapi](https://github.com/optiscaler/fakenvapi) lineage while replacing a single vendor-specific execution model with a normalized multi-input latency architecture.

> [!IMPORTANT]
> This is an experimental compatibility project. It is not affiliated with NVIDIA, AMD, Intel, Khronos, Microsoft, Valve, CodeWeavers, DXVK, VKD3D-Proton, OptiScaler, or any game vendor. Do not replace system NVAPI files.

## Status

Current source line: **0.9.0-vulkanflex-r4.5 + Core Opt7**.

Implemented areas include:

- NVAPI / Reflex-compatible frontend
- Intel XeLL frontend
- AMD Anti-Lag 2 frontend
- `VK_NV_low_latency2` input handling
- VulkanFlex native Vulkan execution/observation
- VKD3D-Proton cooperative transport
- DXVK observer transport
- HybridFusion arbitration with one authoritative pacing owner
- frame-generation cadence/timeline handling
- XRFlex OpenXR timing observation
- AudioFlex WASAPI timing/queue observation

The repository-level qualification suite currently contains **58 host audit/test checks**. Real game/runtime validation is still required before treating a configuration as production-ready.

## Architecture

```text
NVAPI / Reflex ─┐
XeLL ───────────┤
Anti-Lag 2 ─────┼──> normalized observations ──> HybridFusion
VK low latency ─┘                                  │
                                                   v
                                      one execution backend
                                                   │
                         ┌─────────────────────────┼───────────────┐
                         v                         v               v
                    VulkanFlex             native/vendor     LatencyFleX
```

The core rule is:

```text
observe many -> select/freeze semantics -> choose one backend -> keep one pacing owner
```

This prevents multiple latency systems from independently pacing the same frame loop.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the complete ownership and transport model.

## Build

### Requirements

Linux cross-build host:

```bash
# Arch / CachyOS
sudo pacman -S --needed git meson ninja mingw-w64-gcc

# Debian / Ubuntu
sudo apt-get install git meson ninja-build mingw-w64 g++-mingw-w64-x86-64
```

### x64

```bash
git clone https://github.com/AsynKhronos/Vulkanized-Fakenvapi.git
cd Vulkanized-Fakenvapi

meson subprojects download detours
meson setup build-win64 \
  --cross-file build-win64.txt \
  --buildtype release
ninja -C build-win64
```

Output:

```text
build-win64/src/nvapi64.dll
```

For x86, use `build-win32.txt`.

To package both architectures:

```bash
./package-release.sh 0.9.0 ./dist
```

Detailed build and validation notes: [docs/BUILD.md](docs/BUILD.md).

## Tests

Run all repository-level checks:

```bash
./scripts/ci-host.sh
```

GitHub Actions runs the same host qualification plus a MinGW x64 cross-build on pushes and pull requests.

## Configuration

The default runtime configuration is [`fakenvapi.ini`](fakenvapi.ini).

Important policy sections:

```ini
[input]
mode=auto
priority=reflex,xell,antilag2,vk_nv_low_latency2

[hybrid]
xell_fusion=1
vulkan_fusion=1
startup_locked=1

[vulkanflex]
enabled=1
only_native_vulkan=1
vkd3d_bridge=1
```

The default policy prefers native/low-level ownership when capability evidence is sufficient and falls back when it is not. Forcing a backend is not inherently faster or safer.

## Deployment

There is no universal installation path. The DLL can be loaded through game-local proxy setups, OptiScaler deployments, Wine/Proton prefixes, or dedicated test harnesses.

**Do not overwrite operating-system NVIDIA NVAPI DLLs.** Deploy only into the application-specific loading context you control and keep a rollback copy.

Anti-cheat, DRM, launcher policies, and game updates may make DLL proxying unsupported for a particular title. This project is not intended to bypass those systems.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [VulkanFlex](docs/VULKANFLEX.md)
- [XRFlex](docs/XRFLEX.md)
- [AudioFlex](docs/AUDIOFLEX.md)
- [Build and validation](docs/BUILD.md)
- [Changelog](CHANGELOG.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Security-sensitive reports should follow [SECURITY.md](SECURITY.md).

Useful bug reports include the game, graphics API, GPU/driver, Wine/Proton or native Windows environment, translation stack, relevant configuration, logs, and exact reproduction steps.

## Upstream and third-party work

This project builds on or interoperates with work from projects including OptiScaler/fakenvapi, OptiScaler, DXVK, dxvk-nvapi, VKD3D-Proton, Vulkan-Headers, Intel XeSS/XeLL, AMD Anti-Lag 2 SDK, Microsoft Detours, and LatencyFleX-derived code.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for licensing boundaries and bundled third-party material.

## License

Project code retains the upstream **MIT License**. See [LICENSE](LICENSE).

Third-party files remain under their respective licenses. Vendor and API names are used only for technical identification; no endorsement or affiliation is implied.
