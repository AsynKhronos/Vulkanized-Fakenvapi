# Contributing to Vulkanized-Fakenvapi

Contributions are welcome, but changes to latency, synchronization, capability detection, or hot paths must be
reviewable and measurable.

## Before opening a pull request

1. Keep the change narrowly scoped.
2. Explain the problem before explaining the solution.
3. Preserve upstream copyright and license notices.
4. Add or update tests where practical.
5. Do not claim performance improvements without measurements.
6. Avoid adding dependencies unless they solve a measured problem.

## Development principles

- Vulkan is the primary graphics API target.
- Linux / Wine / Proton behavior is first-class.
- Capability detection and backend activation should remain separate.
- Runtime fallback must be deterministic.
- Hot-path allocations should be avoided unless justified.
- Logging and overlays must not materially disturb frame pacing.
- Unsupported states should fail clearly rather than silently selecting an unsafe path.

## Build

See [docs/building.md](docs/building.md).

## Pull request checklist

A pull request should state:

- **Problem:** What is wrong or missing?
- **Change:** What did you change?
- **Compatibility:** Which games, GPUs, drivers, Wine/Proton versions, or backends are affected?
- **Validation:** What tests did you run?
- **Performance:** Is the hot path affected? If yes, include before/after data.
- **Fallback:** What happens if the new path is unavailable?

## Performance changes

Where relevant, include:

- CPU model
- GPU / driver
- compiler and version
- build type
- benchmark method
- sample count
- median / percentile data
- before and after results

A one-off FPS screenshot is not sufficient evidence for a performance claim.

## Coding style

Until a project-specific style guide is frozen:

- follow the style of surrounding code,
- prefer clear ownership and lifetime semantics,
- avoid unnecessary abstraction,
- avoid hidden global state,
- use comments for *why*, not for obvious *what*,
- compile cleanly with the project's warning policy.

## Security-sensitive issues

Do not open a public issue for a vulnerability that could lead to code execution, unsafe DLL loading,
privilege escalation, or similar security impact. Follow [SECURITY.md](SECURITY.md).
