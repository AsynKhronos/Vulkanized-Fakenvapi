# Build and Test Notes — 0.9.0-io-capability-recognizer

## Opt7 qualification

Core Opt7 adds frame-granular input arbitration, O(1) FG cadence routing, and per-instance synchronized LatencyFleX state. Source qualification is 58/58 repository checks plus 4/4 targeted ASan+UBSan. A real MinGW/Windows DLL build remains mandatory before target-platform release sign-off.

0.9 is based on `0.8.0-low-level-execution-r1`. It changes startup detection/routing evidence only; the 0.8 low-level execution backends and startup-locked Hybrid semantics remain intact.

## Required recognizer pass

```bash
./scripts/audit-io-recognizer.sh
./scripts/test-io-recognizer.sh
./scripts/test-policy-engine.sh
./scripts/audit-low-level-execution.sh
./scripts/audit-hybrid-xell.sh
./scripts/test-hybrid-fusion.sh
./scripts/audit-input-arbitration.sh
./scripts/audit-input-frontends.sh
./scripts/audit-gcc-mingw-compat.sh
./scripts/audit-meson-libraries.sh
./scripts/test-vulkan-device-registry.sh
```

Expected D3D12 startup diagnostics now include one line per candidate, for example:

```text
Output detector: api=d3d12, backend=amd_anti_lag, state=available, confidence=98, evidence=...
Output detector: api=d3d12, backend=antilag2, state=available|unknown, confidence=..., evidence=...
Output detector: api=d3d12, backend=xell, state=available|unknown, confidence=..., evidence=...
Output detector: api=d3d12, backend=latencyflex, state=available, confidence=50, evidence=...
```

After the one-time Hybrid startup lock, expect:

```text
Input detector frozen: reflex=q.../e.../m..., vkll2=q.../e.../m..., xell=q.../e.../m..., al2=q.../e.../m...
```

The evidence mask is diagnostic provenance, not a second selector. `Capability`/`Context` alone never create freshness; real marker/Sleep activity is required.

## Fresh MinGW x64 target build

```bash
rm -rf build-win64 subprojects/detours
meson subprojects download detours
meson setup build-win64 --cross-file build-win64.txt --buildtype release

./scripts/audit-io-recognizer.sh
./scripts/test-io-recognizer.sh
./scripts/audit-low-level-execution.sh
./scripts/audit-gcc-mingw-compat.sh
./scripts/audit-meson-libraries.sh

set -o pipefail
ninja -C build-win64 -v 2>&1 | tee build-win64-0.9.0-io-capability-recognizer.log
```

Expected binary: `build-win64/src/nvapi64.dll`. Target-build-green still requires this real MinGW link.

---

# Build and Test Notes — 0.8.0-low-level-execution

0.8 is based on `0.7.0-startup-locked-hybrid-xell-r3`. It preserves startup-locked per-aspect Hybrid ownership and adds a targeted low-level D3D12 execution path through vkd3d-proton. Host tests validate policy, startup locking and source-level invariants; a real MinGW target build is still required before build sign-off.

## Required host/audit pass

```bash
./scripts/audit-low-level-execution.sh
./scripts/audit-hybrid-xell.sh
./scripts/test-hybrid-fusion.sh
./scripts/audit-antilag2-output.sh
./scripts/audit-capability-emulation.sh
./scripts/audit-input-arbitration.sh
./scripts/audit-latencyflex-lifecycle.sh
./scripts/audit-vulkan-low-latency2.sh
./scripts/audit-input-frontends.sh
./scripts/audit-policy-hotpath.sh
./scripts/audit-gcc-mingw-compat.sh
./scripts/audit-meson-libraries.sh
./scripts/audit-detours-toolchain.sh
./scripts/test-policy-engine.sh
./scripts/test-vulkan-device-registry.sh
```

## Recommended AMD/vkd3d runtime configuration

```ini
[general]
allow_fallback=1
runtime_switching=1

[input]
mode=auto
priority=reflex,xell,antilag2,vk_nv_low_latency2
minimum_quality=60

[output]
d3d12=auto

[hybrid]
xell_fusion=1
startup_locked=1
startup_observations=64
aspect_minimum_quality=60

[vulkan]
prefer_native_extensions=1
```

Expected low-level success diagnostics:

```text
Low-level D3D12 backend: direct VK_AMD_anti_lag via vkd3d-proton interop
LowLatency backend: amd_anti_lag for d3d12
Locked low-latency pacing source for session: ...
Hybrid startup lock: ...
Hybrid frozen aspects: ...
```

If the vkd3d interop, extension, feature or function gate fails, Auto routing must fall through cleanly to XeLL/Anti-Lag2/LatencyFlex. With `prefer_native_extensions=0`, the direct `amd_anti_lag` D3D12 candidate must not be attempted at all.

## Fresh MinGW x64 target build

```bash
rm -rf build-win64 subprojects/detours
meson subprojects download detours
meson setup build-win64 --cross-file build-win64.txt --buildtype release

./scripts/audit-low-level-execution.sh
./scripts/audit-hybrid-xell.sh
./scripts/test-hybrid-fusion.sh
./scripts/audit-gcc-mingw-compat.sh
./scripts/audit-meson-libraries.sh
./scripts/test-policy-engine.sh

set -o pipefail
ninja -C build-win64 -v 2>&1 | tee build-win64-0.8.0-low-level-execution.log
```

Expected binary: `build-win64/src/nvapi64.dll`. Do not mark 0.8 target-build-green until this real cross build links successfully.

---

# Build and Test Notes — 0.5.0-capability-emulation

## Validated baselines

- `0.1.0-foundation-r8`: verified GCC/MinGW x64 foundation.
- `0.2.0-policy`: full x64 MinGW compile/link PASS.
- `0.3.0-inputs-r1`: full x64 MinGW compile/link PASS after the AMD DX12 header-order repair.

`0.5.0-capability-emulation` is based on the runtime-tested 0.4.x line and adds game-facing Streamline Reflex and XeLL capability hooks. It requires a fresh MinGW target build before it can be declared build-valid.

## Host-native validation

```bash
./scripts/test-policy-engine.sh
./scripts/audit-capability-emulation.sh
./scripts/audit-policy-hotpath.sh
./scripts/audit-input-frontends.sh
./scripts/audit-vulkan-low-latency2.sh
./scripts/audit-detours-toolchain.sh
./scripts/audit-capability-emulation.sh
./scripts/audit-meson-libraries.sh
./scripts/audit-gcc-mingw-compat.sh
./scripts/test-vulkan-device-registry.sh
```

The Vulkan registry test covers per-device state, timeline signalling, generic semaphore signalling and queue-to-device registration. The Vulkan frontend audit checks extension exposure, proc-address interception, all five `VK_NV_low_latency2` commands, delayed loader discovery, dependency gating, asynchronous sleep signalling, NULL sleep-mode disable semantics and device-group rejection.

## Fresh target build on CachyOS/Arch

```bash
rm -rf build-win64 subprojects/detours
meson subprojects download detours

meson setup build-win64 \
  --cross-file build-win64.txt \
  --buildtype release

./scripts/audit-detours-toolchain.sh
./scripts/audit-meson-libraries.sh
./scripts/audit-gcc-mingw-compat.sh
./scripts/audit-policy-hotpath.sh
./scripts/audit-input-frontends.sh
./scripts/audit-vulkan-low-latency2.sh
./scripts/test-policy-engine.sh
./scripts/test-vulkan-device-registry.sh

set -o pipefail
ninja -C build-win64 -v 2>&1 | tee build-win64-0.5.0-capability-emulation.log
```

Expected output: `build-win64/src/nvapi64.dll`.

## Runtime smoke

1. Start a Vulkan title where `vulkan-1.dll` loads after `nvapi64.dll`; log must report `Vulkan delayed-loader watcher installed`, then `Vulkan hooks installed`.
2. On an AMD Vulkan driver with the required timeline/present-ID dependencies and `[vulkan] expose_nv_low_latency2=auto`, the game should see `VK_NV_low_latency2`, while the driver-facing device create must still succeed.
3. When the game uses low-latency2 markers, selected input should become `vk_nv_low_latency2` after the quality threshold is met.
4. With `prefer_native_extensions=1` on a native NVIDIA implementation, log should report `native pass-through` and no translated Vulkan backend should be driven in parallel.
5. Verify `vkLatencySleepNV` returns without blocking the game thread and the supplied timeline semaphore is signalled after the selected backend pacing step.
6. Verify only one selected input and one pacing output remain active during mixed instrumentation.

### 0.5.0 capability-emulation audit

```bash
./scripts/audit-capability-emulation.sh
```

Runtime validation should confirm game-facing menu availability independently
from the selected output backend. Expected diagnostics include:

```text
Streamline Reflex game-facing capability emulation hooked
Streamline capability: slIsFeatureSupported(Reflex) -> supported (...)
Streamline capability: ReflexState.lowLatencyAvailable forced true (...)
XeLL game-facing capability: synthetic D3D12 context created for <game module>
```


## 0.5.1 Anti-Lag 2 output test

Run `./scripts/audit-antilag2-output.sh`. For a forced D3D12 Anti-Lag 2 runtime test use:

```ini
[general]
allow_fallback=1

[output]
d3d12=antilag2

[backend_order]
d3d12=antilag2,xell,latencyflex
```

On CachyOS/Proton-CachyOS enable the Mesa Anti-Lag layer with `ENABLE_LAYER_MESA_ANTI_LAG=1`. The expected log is `AntiLag 2 DX12 initialized via vkd3d-proton/VK_AMD_anti_lag` followed by `LowLatency backend: antilag2 for d3d12`.


## 0.7.0-r1 Startup-Locked Hybrid XeLL test

Run before the MinGW target build:

```bash
./scripts/audit-hybrid-xell.sh
./scripts/test-hybrid-fusion.sh
```

Recommended runtime configuration:

```ini
[output]
d3d12=auto

[hybrid]
xell_fusion=1
startup_locked=1
startup_observations=64
aspect_minimum_quality=60
```

Expected routing is `LowLatency backend: xell for d3d12`. During warmup no frontend should drive XeLL Sleep. After the one-time decision, expect `Locked low-latency pacing source for session: ...` followed by `Hybrid startup lock: ...`. No later source-owner changes should occur.

## Hot-path architecture audit

```bash
./scripts/test-fixed-mpmc-ring.sh
scripts/audit-zero-allocation-hotpath.sh
```

This verifies the fixed-capacity atomic registry, published backend fast paths,
frozen Hybrid marker route, packed Anti-Lag control state, and release hot-log policy.

## R3.2 translation-stack boot regression

Run:

```bash
./scripts/audit-vulkan-transport-ownership.sh
./scripts/audit-vulkanflex.sh
./scripts/audit-gcc-mingw-compat.sh
```

For a Wine/Proton D3D12 title with `[vulkanflex] only_native_vulkan=1`, expected startup diagnostics now include `Vulkan transport bypass:` and must **not** include `Vulkan hooks installed (low-latency frontends + VulkanFlex WSI bridge)` for that translation-stack session. The game should continue through VKD3D/DXVK device creation with Vulkan dispatch ownership left to the translation layer.

- `scripts/audit-vkd3d-vulkanflex-bridge.sh` validates the R3.3 cooperative D3D12/vkd3d-proton bridge and no-submit/no-detour ownership contract.

## R3.4 executor arbitration / FrameToken regression

Run the complete host suite and architecture audits:

```bash
export TERM=xterm
for s in scripts/test-*.sh scripts/audit-*.sh; do bash "$s"; done
```

The frozen R3.4 source tree expects **23/23** scripts to pass (5 executable host tests + 18 architecture/toolchain audits). `test-hybrid-fusion.sh` includes deliberately unrelated Anti-Lag2 and Reflex native IDs and requires both to resolve to one monotonic Canonical FrameToken. `audit-vulkanflex-executor-arbitration.sh` verifies that D3D12 Auto prefers XeLL/Anti-Lag2 before VulkanFlex fallback, that VKD3D VulkanFlex observer mode cannot sleep, and that no observer heap allocation or Vulkan queue submission was introduced.

For Ghost of Tsushima / other VKD3D titles with XeLL available, expected startup is now conceptually:

```text
Vulkan transport bypass: ...
VulkanFlex VKD3D bridge: ...
VulkanFlex initialized: role=observer, transport=vkd3d-d3d12 ...
VulkanFlex translation observer attached behind D3D12 executor: xell
LowLatency backend: xell for d3d12
Locked low-latency pacing source for session: ...
```

The absence of a VulkanFlex executor log is intentional in this case. There must still be only one CPU wait owner.

### R3.4.1 canonical observer ABI audit

`./scripts/audit-canonical-observer-interface.sh` verifies that canonical observer calls are declared on `LowLatencyTech` and overridden by VulkanFlex, preventing the MinGW `marked override, but does not override` failure observed in the external win64 build.

## R3.5 DXVK cooperative bridge validation

Source-side regression suite: **25/25 PASS**. The new DXVK audit verifies the public COM IIDs/method prefixes, DXVK delegate role, D3D11 FrameToken transport classification, and the invariant that VulkanFlex performs no `vkQueueSubmit*` and no DXVK submission-queue locking. A real MinGW x64 DLL build is still required on the target CachyOS toolchain because this packaging environment has no MinGW cross compiler.

## R3.6 present precision validation

Run:

```bash
export TERM=xterm
for s in scripts/*.sh; do bash "$s"; done
```

The R3.6 source tree expects **27/27** scripts to pass. `test-vulkan-present-precision.sh` validates VF0→VF4 selection including the swapchain opt-in requirements for wait2/timing. `audit-vulkan-present-precision.sh` enforces the R3.6 safety contract: no present-precision extension injection, no swapchain-flag mutation, timeout-0 wait sensors only, no Vulkan queue submission, and retained CPU-present fallback.

The packaging environment still lacks a MinGW cross compiler. A real CachyOS `x86_64-w64-mingw32-g++` compile/link remains mandatory before runtime sign-off. Native Vulkan runtime logs should show the device capability snapshot and one-way `VulkanFlex present precision upgraded: VF...` diagnostics as stronger per-swapchain evidence appears.


## R3.8 frame-generation dual-timeline validation

Run:

```bash
./scripts/test-frame-generation-timeline.sh
./scripts/audit-frame-generation-dual-timeline.sh
```

`test-frame-generation-timeline.sh` validates independent monotonic render/presentation sequences, 1:N mapping, per-render generation reset, interpolation metadata, reverse lookup, startup-locked source ownership and epoch invalidation. `audit-frame-generation-dual-timeline.sh` verifies that `Fake_InformPresentFG` routes through HybridFusion, the observer ABI is polymorphic, storage is fixed, and the presentation path cannot invoke a pacing wait.

The R3.8 source tree expects **31/31** shell tests/audits to pass before packaging. A real MinGW-x64 DLL compile/link remains a separate target-platform sign-off.

## R3.7 queue-pressure validation

Run:

```bash
export TERM=xterm
for s in scripts/*.sh; do bash "$s"; done
```

The R3.7 source tree expects **29/29** scripts to pass. `test-vulkan-queue-pressure.sh` validates Auto warmup/trust, fallback rejection, VF2+ gating, target backlog behavior, CPU-ahead amplification and the configured delay ceiling. `audit-vulkan-queue-pressure.sh` enforces native-executor-only operation, one existing wake target, no `vkQueueSubmit*`, and timeout-0 present-wait sensors.

Recommended first runtime configuration:

```ini
[vulkanflex]
present_precision=auto
queue_pressure=auto
queue_target_presents=1
queue_max_delay_us=2000
```

Expected native-Vulkan diagnostics include `VulkanFlex queue governor armed:` after sufficient precise completions. Trace logging can expose `VulkanFlex queue pressure:` samples. Deinit reports aggregate `queue_events`, added `queue_delay_us`, and maximum observed pending presentations. If the governor never arms, leave it fail-open and inspect the VF tier / fallback ratio before forcing `queue_pressure=1`. A real MinGW x64 compile/link is still mandatory on the CachyOS target toolchain.
## R3.9 Vulkan capability validation

Run:

```bash
./scripts/test-vulkan-capabilities.sh
./scripts/audit-vulkan-capabilities.sh
./scripts/test-vulkan-device-registry.sh
```

`test-vulkan-capabilities.sh` compiles the transport-neutral capability engine with `-Wall -Wextra -Werror` and validates Vulkan 1.4 support, the Vulkan-1.3 promotion boundary, optional EDS2 bits, all-mask EDS3 semantics, exact enabled-state merge and support-only unknown state. `audit-vulkan-capabilities.sh` verifies native/VKD3D/DXVK wiring and rejects rendering/queue mutation from the capability engine. The full frozen R3.9 source tree expects **33/33** shell tests/audits to pass before packaging. A real CachyOS/MinGW x64 DLL compile/link remains a separate mandatory target-platform sign-off.

## R4.0 structural-stall validation

`test-vulkan-structural-stall.sh` compiles the pure classifier with `-Wall -Wextra -Werror` and validates warmup, stale evidence, non-outlier rejection, forced mode and baseline EWMA behavior. `test-latencyflex-structural-compensation.sh` verifies that a known external pause can be collapsed while LatencyFleX stays in the real clock domain. `audit-vulkan-structural-stall.sh` verifies hook/observer wiring and rejects GPU-command or queue-submission behavior from the classifier.

The frozen R4.0 source tree expects **36/36** shell tests/audits to pass. A real CachyOS/MinGW x64 DLL compile/link remains a separate mandatory target-platform sign-off.

## R4.0.1 authoritative transport-truth validation

`test-transport-truth.sh` compiles the lock-free state machine with `-std=c++23 -Wall -Wextra -Werror` and validates weak heuristic, confirmed VKD3D/DXVK, full native-Wait/WSI ownership and mixed-transport transitions. `audit-authoritative-transport-truth.sh` verifies that direct proof is wired from the native Vulkan, VKD3D and DXVK paths; suspected translation cannot claim authoritative transport; `native-vulkan-explicit` disables WSI-derived features; and translation-shadow Vulkan callbacks cannot drive a second sleep.

The frozen R4.0.1 source tree expects **38/38** shell tests/audits to pass. A real CachyOS/MinGW x64 DLL compile/link remains a separate mandatory target-platform sign-off. Runtime sign-off should include the three observed classes: native Vulkan full-WSI, D3D12/VKD3D with XeLL + VulkanFlex observer, and the Arknights-style weak-bypass/native-explicit path.

### R4.2 host checks

```sh
scripts/test-openxr-canonical-clock.sh
scripts/test-canonical-clock-math.sh
scripts/audit-openxr-canonical-clock.sh
```

The frozen R4.2 source tree expects **43/43** shell tests/audits to pass. The target MinGW x64 compile/link and OpenXR runtime test remain required on CachyOS/Wine.

### R4.3 AudioFlex host checks

```sh
scripts/test-audioflex-timeline.sh
scripts/audit-audioflex-wasapi.sh
```

The frozen R4.3 source tree expects **45/45** shell tests/audits to pass. `test-audioflex-timeline.sh` compiles the fixed-state timeline and audio-time conversion helpers with `-std=c++23 -Wall -Wextra -Werror`. `audit-audioflex-wasapi.sh` locks the observer contract: exact initialization forwarding, fixed telemetry, no `IAudioRenderClient` writes, no period forcing, no resampling, and no new pacing/sleep path. A real CachyOS/MinGW x64 DLL compile/link and runtime WASAPI log remain separate target-platform sign-offs.

### R4.4 AudioFlex queue-model checks

The frozen R4.4 source tree expects **47/47** executable shell tests/audits to pass. `test-audioflex-queue-model.sh` validates shared-render padding conversion, buffer capacity/fill, separate engine sample-rate handling, clock-position conversion and stale-sample behavior with `-std=c++23 -Wall -Wextra -Werror`. `audit-audioflex-queue-model.sh` enforces getter-only probing, `CoTaskMemFree` ownership for `GetCurrentSharedModeEnginePeriod`, throttled runtime clock refresh and the no-mutation/no-resampling/no-sleep contract. A real CachyOS/MinGW x64 DLL compile/link and runtime WASAPI exercise remain separate target-platform sign-offs.


### R4.5 AudioFlex adaptive-period checks

The frozen R4.5 source tree expects **49/49** executable shell tests/audits to pass. `test-audioflex-period-policy.sh` compiles the candidate selector with `-std=c++23 -Wall -Wextra -Werror` and validates target conversion, fundamental-period alignment, min/max bounds, no period increase and maximum one-step reduction. `audit-audioflex-adaptive-period.sh` enforces recommend-only Auto mode, single-call initialization semantics and the no-render-buffer/no-resampling/no-sleep boundary. Real MinGW x64 compile/link and WASAPI runtime exercise remain target-platform sign-offs.
