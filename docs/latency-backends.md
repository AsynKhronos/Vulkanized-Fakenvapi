# Latency Backends

This document should describe what Vulkanized-Fakenvapi **actually implements**, not merely what an SDK or GPU
vendor supports in theory.

## NVIDIA Reflex-facing path

Reflex-facing game calls can act as an input / synchronization signal. Native Reflex behavior must be kept
conceptually separate from translated or forwarded compatibility paths.

## AMD Anti-Lag 2

Upstream fakenvapi already supports Anti-Lag 2. Vulkanized-Fakenvapi should document:

- whether the path is upstream-compatible or modified,
- platform restrictions,
- GPU requirements,
- frame-generation limitations,
- activation diagnostics.

## Vulkan Anti-Lag+

Upstream fakenvapi also reports Vulkan Anti-Lag+ support. Vulkanized-Fakenvapi should keep this separate from
other Anti-Lag generations where their runtime requirements differ.

## XeLL

XeLL should be represented as an explicit backend with independent:

- capability detection,
- initialization,
- activation,
- shutdown,
- error reporting.

## LatencyFlex

LatencyFlex is a cross-vendor / cross-platform upstream option. Preserve compatibility where practical and
document any behavioral divergence.

## Backend comparison policy

Do not label one backend "best" without reproducible measurements.

Compatibility can vary with:

- GPU generation,
- driver,
- game engine,
- native Reflex integration,
- Wine / Proton version,
- frame generation,
- Vulkan extensions,
- backend implementation quality.
