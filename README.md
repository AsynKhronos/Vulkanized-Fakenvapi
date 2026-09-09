# Vulkanized-Fakenvapi

**A Vulkan-focused fakenvapi fork for Linux gaming with configurable low-latency backend selection, runtime capability detection, and a cleaner latency-policy architecture.**

> [!WARNING]
> **Early development.** Vulkanized-Fakenvapi is not yet a stable drop-in replacement for every fakenvapi setup.  
> Features marked *planned* or *in development* should be treated accordingly.

## Overview

Vulkanized-Fakenvapi is an independent continuation and experimental fork of the original
[optiscaler/fakenvapi](https://github.com/optiscaler/fakenvapi), with a narrower engineering focus:

- Linux gaming
- Wine / Proton
- Vulkan
- low-latency API translation and forwarding
- explicit backend selection
- runtime capability detection
- transparent backend reporting
- minimal additional overhead

The aim is not merely to add more latency technologies. The aim is to build a **clean latency-policy layer**
that can determine what is actually available at runtime and select an appropriate backend automatically,
or honor an explicit user choice.

## Upstream

This project is derived from the original:

**[optiscaler/fakenvapi](https://github.com/optiscaler/fakenvapi)**

The upstream repository is MIT-licensed and was archived by its owner on **July 4, 2026** after fakenvapi
was integrated into the wider [OptiScaler](https://github.com/optiscaler/OptiScaler) project.

The upstream fakenvapi project already supports technologies including:

- LatencyFlex
- AMD Anti-Lag 2
- Vulkan Anti-Lag+
- XeLL

and automatically selects some of those paths when available.

Vulkanized-Fakenvapi therefore does **not** claim that those technologies originated here. Its intended
differentiation is a more explicit Linux/Vulkan-focused architecture, configurable backend policy,
capability reporting, and transparent selection/fallback behavior.

This project is **not affiliated with or officially supported by the OptiScaler team**. Please report
Vulkanized-Fakenvapi-specific problems here rather than to the upstream maintainers.

## Project goals

### Vulkan first

Vulkan is the primary graphics API target. The project should keep Vulkan-specific latency behavior explicit
and testable instead of hiding it behind unnecessary abstraction.

### Configurable latency backends

The long-term configuration model is intended to support both automatic and manual selection.

```ini
[latency]
backend = auto

# Intended policies:
# auto
# reflex
# antilag
# xell
# latencyflex
# off
```

Exact names and semantics may change before the first stable release.

### Automatic backend selection

The policy engine should distinguish between:

- compiled-in support
- runtime availability
- active backend
- fallback backend
- unavailable backend

Backend ranking must ultimately be based on **measured compatibility and latency**, not assumptions.

A future configurable priority policy may look like:

```ini
[latency]
backend = auto
fallback = true
backend_priority = reflex, antilag, xell, latencyflex
```

### Transparent runtime state

A small optional startup status notification is planned so users can immediately see whether the project
loaded and which latency path is active.

Example concept:

```text
Vulkanized-Fakenvapi

Vulkan        OK
Reflex        available
Anti-Lag      available
XeLL          available

Active backend: Reflex
```

The overlay should be temporary, optional, and low-overhead.

## Architecture

```text
Game
 │
 │ Reflex / NVAPI-facing latency calls
 ▼
Vulkanized-Fakenvapi
 │
 ├── Capability Detection
 ├── Configuration
 ├── Backend Policy
 ├── Runtime State
 └── Diagnostics / Startup Overlay
 │
 ├──────────────┬──────────────┬──────────────┬──────────────┐
 ▼              ▼              ▼              ▼              ▼
Reflex      Anti-Lag      Vulkan AL+         XeLL       LatencyFlex
 │              │              │              │              │
 └──────────────┴──────────────┴──────────────┴──────────────┘
                                │
                              Vulkan
```

See [docs/architecture.md](docs/architecture.md).

## Project status

| Area | Status |
|---|---|
| Linux-first direction | Core goal |
| Vulkan-first architecture | Core goal |
| Upstream fakenvapi baseline | Upstream |
| Modern Vulkan headers | In development |
| Backend abstraction | In development |
| Runtime capability detection | In development |
| Manual backend selection | Planned |
| Automatic backend policy | Planned |
| Configurable priority/fallback | Planned |
| Startup capability overlay | Planned |
| Structured diagnostics | Planned |
| Performance benchmarks | Planned |
| Stable release | Not yet |

Status entries describe **Vulkanized-Fakenvapi**, not upstream fakenvapi or OptiScaler.

## Performance philosophy

Low-latency software should not create unnecessary latency of its own.

```text
Measure
  ↓
Identify bottleneck
  ↓
Optimize
  ↓
Measure again
```

The project does not assume that custom allocators, alternative standard libraries, task schedulers, or other
dependencies are automatically faster. Performance-oriented changes should be justified by reproducible
measurements.

Primary concerns:

- hot-path CPU overhead
- allocations
- synchronization
- frame-time variance
- backend dispatch cost
- logging overhead
- startup overhead

## Intended toolchain

```text
Language:          Modern C++
Primary OS:        Linux
Graphics API:      Vulkan
Build system:      Meson
Primary compiler:  Clang
Secondary compiler: GCC
```

## Roadmap

### Phase 1 — Foundation

- [ ] Freeze a clean upstream baseline
- [ ] Audit existing latency paths
- [ ] Modernize Vulkan headers
- [ ] Establish clean Clang builds
- [ ] Preserve GCC compatibility where practical
- [ ] Establish regression tests

### Phase 2 — Backend architecture

- [ ] Common latency backend interface
- [ ] Separate capability detection from activation
- [ ] Separate Reflex-facing input from execution backend
- [ ] Clean Vulkan-oriented Anti-Lag path
- [ ] Explicit XeLL backend integration
- [ ] Preserve compatible LatencyFlex behavior

### Phase 3 — Policy engine

- [ ] `auto` backend selection
- [ ] manual backend override
- [ ] configurable fallback behavior
- [ ] configurable backend priority
- [ ] deterministic backend selection
- [ ] structured capability reporting

### Phase 4 — User visibility

- [ ] startup status notification
- [ ] active-backend reporting
- [ ] improved logging
- [ ] concise unsupported-path diagnostics

### Phase 5 — Validation

- [ ] unit tests
- [ ] Vulkan validation testing
- [ ] Wine / Proton testing
- [ ] vendor compatibility testing
- [ ] CPU-overhead benchmarks
- [ ] frame-time benchmarks
- [ ] latency measurements
- [ ] compatibility matrix

## Documentation

- [Architecture](docs/architecture.md)
- [Configuration](docs/configuration.md)
- [Latency backends](docs/latency-backends.md)
- [Compatibility](docs/compatibility.md)
- [Building](docs/building.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)

## Reporting bugs

Please use the issue forms and provide at least:

```text
Game:
Game version:
GPU:
GPU architecture:
GPU driver:
Distribution:
Kernel:
Wine / Proton version:
DXVK / VKD3D-Proton version:
Vulkanized-Fakenvapi commit/release:
Configured latency backend:
Detected / active latency backend:
Frame Generation:
Relevant configuration:
Relevant log output:
```

Reports that contain only "it does not work" are generally not actionable.

## Contributing

Contributions are welcome, particularly around:

- Vulkan
- NVAPI compatibility
- Wine / Proton
- DXVK
- VKD3D-Proton
- AMD latency APIs
- NVIDIA Reflex-facing integration
- Intel XeLL
- latency measurement
- C++ performance engineering
- compatibility testing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Acknowledgements

### fakenvapi

Original project: **[optiscaler/fakenvapi](https://github.com/optiscaler/fakenvapi)**

Vulkanized-Fakenvapi retains attribution to the upstream project and its contributors.

### OptiScaler

fakenvapi was later integrated into **[optiscaler/OptiScaler](https://github.com/optiscaler/OptiScaler)**.

### dxvk-nvapi

The upstream fakenvapi project states that it was inspired by / based on
**[jp7677/dxvk-nvapi](https://github.com/jp7677/dxvk-nvapi)**.

## License

Vulkanized-Fakenvapi is distributed under the **MIT License**.

The original fakenvapi copyright notice is preserved in [LICENSE](LICENSE). Existing source-file copyright and
license notices from upstream code must not be removed.

Third-party libraries, SDKs and components remain subject to their respective licenses.

## Disclaimer

Vulkanized-Fakenvapi is an independent open-source project.

It is not affiliated with, endorsed by, sponsored by, or officially supported by NVIDIA, AMD, Intel, Valve,
Microsoft, OptiScaler, or the OptiScaler maintainers.

Product and technology names are trademarks of their respective owners.

Do not use game-injection or compatibility-layer modifications in online / anti-cheat-protected games unless
you understand the game's rules and the associated account risk.
