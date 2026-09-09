# GitHub Repository Setup Checklist

Use this once after copying the pack into the repository.

## About

**Description**

> Vulkan-focused fakenvapi fork for Linux gaming with configurable low-latency backend selection across NVIDIA Reflex, AMD Anti-Lag 2 and XeLL.

**Topics**

Recommended core topics:

- vulkan
- linux-gaming
- fakenvapi
- nvapi
- proton
- wine
- nvidia-reflex
- amd-antilag
- xell
- low-latency

Optional additional topics:

- dxvk
- vkd3d-proton
- latency
- frame-generation
- gaming

## Features

Recommended:

- Issues: ON
- Discussions: optional
- Projects: optional
- Wiki: OFF unless there is a concrete reason to maintain separate documentation

Keep technical documentation in `docs/`.

## Security

Enable, if available:

- Private vulnerability reporting
- Dependabot alerts
- Secret scanning where supported
- Branch protection / rulesets for the default branch

## Branch protection / ruleset

For the default branch:

- require pull request before merging once more contributors arrive,
- require CI checks to pass,
- block force pushes,
- block branch deletion.

For a solo early-stage repository, you may temporarily keep direct pushes enabled to avoid unnecessary process.

## Labels

Suggested labels:

- bug
- compatibility
- performance
- latency
- vulkan
- reflex
- anti-lag
- xell
- wine
- proton
- documentation
- enhancement
- good first issue
- help wanted

## Releases

Suggested early versioning:

- v0.1.0-alpha
- v0.2.0-alpha
- v0.5.0-beta
- v1.0.0

Do not publish a `v1.0.0` until configuration behavior, fallback semantics, and supported runtime combinations are
stable enough to document.

## Social preview / logo

Intentionally not included in this pack. Add branding later when the visual identity is finalized.

## CI warning

`.github/workflows/ci.yml` assumes a Meson build and installs only generic Ubuntu Vulkan/build dependencies.
Adjust the dependency list and Meson options to match the actual source tree before requiring the workflow as
a protected status check.
