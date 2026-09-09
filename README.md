# Vulkanized-Fakenvapi

**A Vulkan-focused evolution of fakenvapi for Linux gaming, with configurable low-latency backend selection, runtime capability detection, and a cleaner latency-policy architecture.**

> [!IMPORTANT]
> **Vulkanized-Fakenvapi is currently under active development.**
>
> The project is not yet intended as a drop-in production replacement for every fakenvapi configuration. Features marked as planned or experimental should be treated accordingly.

---

## Overview

**Vulkanized-Fakenvapi** is an independent continuation and experimental fork of the original [optiscaler/fakenvapi](/optiscaler/fakenvapi), focused primarily on:

* Linux
* Wine / Proton
* Vulkan
* low-latency API translation and forwarding
* explicit backend selection
* automatic runtime capability detection
* minimal runtime overhead
* modernized Vulkan and C++ infrastructure

The goal is not simply to add more latency technologies.

The goal is to provide a **clean policy layer** that can determine which low-latency mechanism is actually available for a game, GPU, driver and runtime environment — and then select the most appropriate implementation automatically or according to explicit user configuration.

---

## Upstream

This project is derived from the original:

### [optiscaler/fakenvapi](/optiscaler/fakenvapi)

The original fakenvapi project was created and maintained by the **OptiScaler developers**, including **Michał Lewandowski / FakeMichau** and contributors.

The upstream project provides an NVAPI compatibility implementation with support for technologies including:

* AMD Anti-Lag 2
* Vulkan Anti-Lag
* XeLL
* LatencyFlex
* Reflex-facing NVAPI interception

The original standalone fakenvapi repository was archived in July 2026 after the project was integrated into:

### [OptiScaler](/optiscaler/OptiScaler)

Vulkanized-Fakenvapi exists because its development goals are intentionally more specialized: **Linux, Vulkan, runtime backend control and low-latency experimentation**.

This project is **not affiliated with or officially supported by the OptiScaler team**.

Please report Vulkanized-Fakenvapi-specific problems here rather than to the upstream OptiScaler maintainers.

---

## Project Goals

Vulkanized-Fakenvapi is being designed around several principles.

### 1. Vulkan First

Vulkan is the primary graphics API target.

The project aims to keep Vulkan-specific latency handling explicit instead of hiding platform-specific behavior behind unnecessarily broad abstractions.

---

### 2. Configurable Latency Backends

Users should be able to explicitly choose which low-latency implementation is preferred when multiple technologies are available.

Conceptually:

```ini
[latency]

backend = auto

# Possible policies:
# auto
# reflex
# antilag
# xell
# latencyflex
# off
```

Exact backend names and semantics may change before the first stable release.

---

### 3. Automatic Backend Selection

`auto` mode is intended to inspect runtime capabilities and select an appropriate latency implementation.

Conceptually:

```text
Game
 │
 │ Low-latency / Reflex-facing calls
 ▼
Vulkanized-Fakenvapi
 │
 ├── Capability Detection
 ├── Configuration
 ├── Backend Policy
 └── Runtime State
 │
 ├──────────────┬──────────────┬──────────────┐
 ▼              ▼              ▼              ▼
Reflex       AMD Anti-Lag     XeLL       LatencyFlex
 │              │              │              │
 └──────────────┴──────────────┴──────────────┘
                        │
                      Vulkan
```

Backend selection must depend on actual runtime support.

It must **not** assume that a technology is available merely because a particular GPU vendor is detected.

---

## Backend Policy

The long-term design allows both automatic and manual selection.

Example:

```ini
[latency]

backend = auto
fallback = true
```

An optional configurable priority policy is also planned:

```ini
backend_priority = reflex, antilag, xell, latencyflex
```

This is intentionally configurable rather than permanently hard-coded.

The optimal backend can depend on:

* GPU architecture
* Vulkan driver
* Wine / Proton version
* game engine
* native game latency implementation
* frame-generation implementation
* available Vulkan extensions
* latency API integration quality

Backend ranking should ultimately be based on **measured latency and compatibility**, not assumptions.

---

## Runtime Capability Detection

One of the central goals of Vulkanized-Fakenvapi is making backend availability visible.

The runtime should be able to distinguish between:

```text
SUPPORTED
AVAILABLE
ACTIVE
FALLBACK
UNAVAILABLE
```

These are not equivalent states.

For example, a backend may be compiled into Vulkanized-Fakenvapi but unavailable because the required driver extension is missing.

---

## Startup Status Overlay

A small optional startup notification is planned.

Example:

```text
Vulkanized-Fakenvapi

Vulkan        ✓
Reflex        ✓
AMD Anti-Lag  ✓
XeLL          ✓

Active latency backend:
AMD Anti-Lag
```

The overlay is intended to be:

* minimal
* temporary
* non-interactive
* low-overhead
* useful for troubleshooting

It should disappear automatically after a configurable interval.

Example configuration:

```ini
[overlay]

startup = true
duration_ms = 3000
```

The purpose is simple:

> The user should immediately know whether Vulkanized-Fakenvapi loaded and which latency path is actually being used.

---

## Planned Configuration

The configuration system is intended to expose substantially more control than a simple enable/disable switch.

Example:

```ini
[vulkanized_fakenvapi]

enabled = true

[latency]

backend = auto
fallback = true
backend_priority = reflex, antilag, xell, latencyflex

[overlay]

startup = true
duration_ms = 3000

[logging]

level = info
```

This format is currently a design target and may change while the configuration layer is implemented.

---

## Linux / Proton Focus

Linux is a first-class target rather than a secondary compatibility environment.

Primary development scenarios include:

* Steam Proton
* custom Proton builds
* Wine
* Lutris
* DXVK
* VKD3D-Proton
* native Vulkan components where applicable

The project specifically aims to make latency backend behavior easier to understand and control under Linux gaming stacks.

---

## Vulkan

Vulkanized-Fakenvapi aims to track modern Vulkan headers and use Vulkan capabilities directly where appropriate.

Relevant technologies may include Vulkan low-latency extensions such as:

```text
VK_AMD_anti_lag
VK_NV_low_latency2
```

Availability and functionality depend on the actual GPU driver and runtime environment.

Vulkan extension presence alone must not be treated as proof that a complete low-latency path is functional.

---

## Relationship to NVIDIA Reflex

Vulkanized-Fakenvapi does **not** implement NVIDIA Reflex itself.

Depending on the execution environment, Reflex-facing calls may instead be:

* intercepted
* translated
* forwarded
* used as synchronization/input signals for another latency backend

Native NVIDIA Reflex behavior should remain conceptually separate from compatibility or translation paths.

This distinction is important for accurate debugging and latency measurement.

---

## Relationship to AMD Anti-Lag

AMD exposes more than one generation and integration model of Anti-Lag technology.

Vulkanized-Fakenvapi therefore avoids treating every AMD latency mechanism as if it were identical.

The project intends to distinguish between relevant implementations and expose the active path clearly to the user.

Support depends on:

* GPU generation
* driver support
* operating system
* Vulkan extension availability
* game integration
* frame-generation configuration

---

## Relationship to XeLL

XeLL is another candidate low-latency backend.

Vulkanized-Fakenvapi aims to make XeLL explicitly selectable instead of treating it only as an opaque fallback.

Where supported, the runtime should also be able to compare its availability against other latency implementations when operating in automatic mode.

---

## Performance Philosophy

Latency software should not introduce unnecessary latency of its own.

Vulkanized-Fakenvapi therefore follows a measurement-driven optimization policy:

```text
Measure
   ↓
Identify bottleneck
   ↓
Optimize
   ↓
Measure again
```

The project does **not** assume that replacing the C++ standard library, adding a custom allocator, introducing a task scheduler, or adding another dependency automatically improves performance.

Any substantial performance-oriented dependency should require measurable justification.

Primary concerns include:

* hot-path CPU overhead
* allocations
* synchronization
* indirect calls
* frame-time variance
* startup cost
* logging overhead
* backend dispatch overhead

Performance claims should be backed by reproducible measurements.

---

## Toolchain

The intended primary development environment is:

```text
Language:       Modern C++
Primary OS:     Linux
Graphics API:   Vulkan
Build system:   Meson
Primary compiler: Clang
Secondary compiler: GCC
```

Compiler-specific optimizations should not compromise correctness or unnecessarily prevent the project from building with another standards-compliant compiler.

---

## Project Status

| Component                     | Status               |
| ----------------------------- | -------------------- |
| Linux focus                   | 🟢 Core goal         |
| Vulkan-first architecture     | 🟢 Core goal         |
| Original fakenvapi foundation | 🟢 Upstream          |
| Modern Vulkan headers         | 🟡 In progress       |
| Backend abstraction           | 🟡 In progress       |
| Capability detection          | 🟡 In progress       |
| Manual backend selection      | 🟡 Planned           |
| Automatic backend policy      | 🟡 Planned           |
| AMD Vulkan Anti-Lag path      | 🟡 Under development |
| XeLL selection                | 🟡 Under development |
| Reflex path handling          | 🟡 Under development |
| LatencyFlex compatibility     | 🟡 Under evaluation  |
| Startup capability overlay    | 🟡 Planned           |
| Performance benchmarks        | 🟡 Planned           |
| Stable release                | 🔴 Not yet           |

Status indicators describe the **Vulkanized-Fakenvapi project**, not capabilities of upstream fakenvapi or OptiScaler.

---

## Roadmap

### Phase 1 — Foundation

* [ ] Freeze a clean upstream baseline
* [ ] Modernize build configuration
* [ ] Update Vulkan headers
* [ ] Establish Clang as the primary development compiler
* [ ] Preserve GCC compatibility where practical
* [ ] Audit existing latency paths
* [ ] Establish regression tests

### Phase 2 — Backend Architecture

* [ ] Introduce common latency backend interface
* [ ] Separate capability detection from backend activation
* [ ] Cleanly separate Reflex-facing input from execution backend
* [ ] Implement Vulkan-oriented AMD latency path
* [ ] Integrate XeLL backend
* [ ] Preserve compatible LatencyFlex behavior

### Phase 3 — Policy Engine

* [ ] `auto` backend mode
* [ ] manual backend selection
* [ ] configurable fallback behavior
* [ ] configurable backend priority
* [ ] deterministic backend selection
* [ ] structured capability reporting

### Phase 4 — User Visibility

* [ ] startup status notification
* [ ] active-backend reporting
* [ ] capability reporting
* [ ] improved logging
* [ ] concise diagnostics for unsupported configurations

### Phase 5 — Validation

* [ ] unit tests
* [ ] Vulkan validation testing
* [ ] Wine testing
* [ ] Proton testing
* [ ] GPU-vendor compatibility testing
* [ ] CPU-overhead benchmarks
* [ ] frame-time benchmarks
* [ ] latency measurements
* [ ] compatibility matrix

### Phase 6 — Stable Release

A stable release should only be declared once the backend-selection behavior and compatibility rules are sufficiently understood and reproducible.

---

## Compatibility

A formal compatibility matrix will be added as testing progresses.

The intended format is:

| GPU        | Driver      | Runtime | Backend         | Status |
| ---------- | ----------- | ------- | --------------- | ------ |
| AMD RDNA   | Mesa        | Proton  | Vulkan Anti-Lag | TBD    |
| AMD RDNA   | Mesa        | Proton  | XeLL            | TBD    |
| NVIDIA RTX | Proprietary | Proton  | Reflex path     | TBD    |
| Intel Arc  | Mesa        | Proton  | XeLL            | TBD    |

No combination should be marked supported until it has actually been tested.

---

## Building

Build instructions will be finalized once the first Vulkanized-Fakenvapi development baseline is frozen.

The project uses **Meson** and is intended to support **Clang** as its primary compiler.

Until the first development release, build interfaces and dependencies should be considered unstable.

---

## Reporting Bugs

When reporting a problem, please include as much of the following information as possible:

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
Detected latency backend:
Frame Generation:
Relevant configuration:
Relevant log output:
```

A report stating only that "latency does not work" is generally not sufficient to diagnose backend-selection or translation problems.

---

## Contributing

Contributions are welcome, particularly in:

* Vulkan development
* NVAPI compatibility
* Wine / Proton
* DXVK
* VKD3D-Proton
* latency measurement
* AMD GPU development
* NVIDIA Reflex integration
* Intel XeLL
* C++ performance engineering
* testing and compatibility validation

Performance-related pull requests should ideally include before/after measurements.

Behavior-changing pull requests should describe:

1. the problem,
2. the affected path,
3. the proposed change,
4. compatibility implications,
5. test results.

---

## Upstream Projects & Acknowledgements

Vulkanized-Fakenvapi would not exist without the work of the projects and developers it builds upon.

### fakenvapi

Original project:

**[optiscaler/fakenvapi](/optiscaler/fakenvapi)**

Created and maintained by the OptiScaler developers and contributors.

Vulkanized-Fakenvapi retains attribution to the original project and its contributors.

### OptiScaler

The original fakenvapi project was later integrated into:

**[optiscaler/OptiScaler](/optiscaler/OptiScaler)**

OptiScaler continues to provide fakenvapi functionality as part of its broader upscaling and frame-generation compatibility system.

### DXVK-NVAPI

The original fakenvapi project itself states that it was inspired by / based on:

**[jp7677/dxvk-nvapi](/jp7677/dxvk-nvapi)**

DXVK-NVAPI provides an alternative NVAPI implementation for Linux gaming environments using DXVK and VKD3D-Proton.

Please respect the licenses and attribution requirements of all upstream and third-party components.

---

## License

Vulkanized-Fakenvapi is distributed under the **MIT License**.

See `LICENSE` for the complete license text.

Because this project is derived from existing MIT-licensed software, original copyright and license notices from upstream code must be preserved where required.

Third-party libraries, SDKs and components remain subject to their respective licenses.

---

## Disclaimer

Vulkanized-Fakenvapi is an independent open-source project.

It is **not affiliated with, endorsed by, sponsored by, or officially supported by**:

* NVIDIA
* AMD
* Intel
* Valve
* Microsoft
* the OptiScaler project or its maintainers

Product and technology names are trademarks of their respective owners.

Use this software at your own risk.

Low-latency behavior can vary significantly depending on the game, GPU, driver, Wine/Proton version and frame-generation configuration.

---

## Why Vulkanized-Fakenvapi?

The intended end state is straightforward:

```text
One latency compatibility layer.
Multiple possible backends.
Clear capability detection.
Explicit user control.
Automatic fallback when requested.
Linux and Vulkan as first-class targets.
No guessing about which backend is actually active.
```

That is the problem Vulkanized-Fakenvapi is intended to solve.
