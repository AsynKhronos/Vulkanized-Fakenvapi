## 0.9.0-vulkanflex-r4.5 — VulkanFlex Opt5

## Core Opt7 — arbitration / FG cadence / LatencyFleX

- Make input freshness frame-granular so marker-rich frontends cannot age sparse frontends or inflate quality through callback density.
- Add read-mostly evidence/marker publication and coherent one-watermark arbitration.
- Replace the process-global OOB FG history with a per-thread O(1) 11-bit cadence detector plus bounded lock-free producer migration.
- Update effective FG backend state only on real cadence transitions.
- Move LatencyFleX mode/timeout/reset state per instance and externally synchronize all estimator-context operations.
- Add `audit-core-opt7.sh` and dedicated cadence/router tests.
- Final qualification: 58/58 repository checks plus 4/4 targeted Clang ASan+UBSan.

- Claim native-Vulkan swapchain image/frame mappings before the real `vkQueuePresentKHR` call and complete feedback only after a successful driver return. This prevents a concurrent acquire from recycling an image before the old present consumes its mapping.
- Publish newly created swapchain lanes only after their payload/profile state is initialized, closing the previous partial-initialization visibility window.
- Cache immutable present-precision dispatch/capability state per tracked swapchain lane instead of rescanning the global Vulkan registry on every pending acquire/precise present.
- Replace the precision-ring cursor + unconditional slot exchange with free-slot-first fixed-ring claiming. Occupied samples are displaced only when all eight slots are genuinely busy.
- Reserve a transient publication sentinel so readers never consume a precision slot while its payload is being rewritten.
- Bound per-acquire zero-timeout present-wait probing to four rotating slots, limiting driver-call burst cost while preserving fair progress across the eight-slot ring.
- Call `vkGetPastPresentationTimingEXT` only when a live pending sample actually requested timing metadata.
- Count wait hits only after the caller successfully claims retirement, preventing double statistics/RMW traffic when completion polling races.
- Remove one locked RMW from each present mapping claim by loading/clearing the epoch after the frame-id exchange instead of atomically exchanging both fields.
- Preserve Vulkan ownership: no queue submits, queue-idle waits, command buffers, extension injection, swapchain-flag mutation, or second pacing owner were added.

## 0.9.0-vulkanflex-r4.5 — Hybrid Chain Opt4

- Split `InputSample`, `TriggerFlash`, and `PcLatencyPing` into independent Hybrid ownership aspects so AL2/XeLL input sampling cannot suppress Reflex/VK-LL2 latency markers.
- Require complete startup evidence for paired lifecycle ownership; a source that exposes only one half of Simulation/Render/Present/OOB lifecycle can no longer freeze ownership of the missing half.
- Publish the complete frozen Hybrid ownership matrix as one packed read-mostly routing word for steady-state marker/pacing/FG dispatch.
- Publish each frontend control packet, including its valid-field mask, as one coherent 64-bit atomic snapshot and resolve all fields from one per-frontend generation.
- Replace blind delayed-secondary `bind-to-latest` behavior with bounded monotonic source-domain correlation; ambiguous delayed IDs fail open instead of being time-shifted onto a newer render token.
- Revalidate frame/source/presentation publication words after payload reads to prevent torn tokens during fixed-ring slot reuse.
- Remove redundant locked sequence/generation RMWs already serialized by the token/presentation publication gates, and avoid repeated startup mask RMWs once a bit is set.
- Fix the `aspect_minimum_quality` parser default to use its own policy field.
- Host validation: 53/53 repository scripts PASS; focused Clang ASan+UBSan PASS; GCC `-Wall -Wextra -Wpedantic -Werror` PASS.

## R4.5 - Opt-in adaptive audio period negotiation

- Added an init-time period candidate policy for applications that already call `IAudioClient3::InitializeSharedAudioStream`.
- `adaptive_period=auto` is recommendation-only and never changes the application's requested period.
- `adaptive_period=1` may lower that one application-provided period to a supported target aligned to `fundamentalPeriod` and clamped to `[minPeriod,maxPeriod]`.
- Period negotiation never increases the application period and is capped by `period_max_reduction_percent` (50% default) per initialization.
- Initialization is attempted exactly once. There is deliberately no automatic retry on the same `IAudioClient` after a failed negotiated initialization because WASAPI may subsequently report `AUDCLNT_E_ALREADY_INITIALIZED` even after an initial failure.
- Classic `IAudioClient::Initialize` remains observer-only; R4.5 does not convert it into an `IAudioClient3` initialization path.
- No stream stop/reset/reinitialize loop, render-buffer access, resampling, audio sleep, or graphics/XR ownership change was added.
- Added `audio_period_policy.h`, a C++23 unit test and an ownership/safety audit.

## R4.4 - Audio clock + endpoint queue-latency model

- Added a shared-mode render queue model that converts observed WASAPI padding into queued endpoint audio time while keeping the result explicitly separate from end-to-end speaker/headphone latency.
- Added getter-only post-initialize probing for `GetBufferSize`, `GetStreamLatency`, `GetCurrentPadding`, `GetDevicePeriod`, `IAudioClient3` period capability/current-period queries and `IAudioClock` correlation.
- Added throttled runtime clock refresh from the existing padding path; default cadence is 250 ms rather than one extra clock query per padding sample.
- Kept client stream sample rate separate from the current shared-engine format so a device/mix-format query cannot corrupt padding-to-time conversion.
- Added fixed-state queue statistics, first-valid-model diagnostics and shutdown average/max queued-audio summaries.
- R4.4 remains observer-only: no `InitializeSharedAudioStream` call on behalf of the application, no `SetClientProperties`, no render-buffer access, no resampling, no sleep and no graphics/XR ownership change.
- Added `active_probe`, `queue_model`, and `clock_probe_interval_ms` audio policy controls plus queue-model unit/audit coverage.

## R4.3 - AudioFlex WASAPI observer

- Added a separate `AudioFlex` latency domain rather than coupling audio buffering to VulkanFlex or XRFlex.
- Added classic MMDevice/WASAPI discovery through `CoCreateInstance(CLSID_MMDeviceEnumerator)`, endpoint activation and `IAudioClient`/`IAudioClient3` COM entry points.
- Added fixed-state telemetry for stream format/share mode, requested buffer/period, actual buffer frames, current padding, stream/device periods and IAudioClient3 engine-period range/current period.
- Added passive `IAudioClock::GetFrequency` / `GetPosition` correlation; WASAPI's QPC value is retained in the shared QPC-derived host time domain without claiming end-to-end latency yet.
- Padding and audio-clock hot paths are atomic/fixed-storage and do not log per sample.
- R4.3 forwards all initialization arguments unchanged and never sets a client period, writes an audio render buffer, resamples, sleeps or changes the graphics/XR pacing owner.
- Added `[audio] enabled`, `wasapi_observer`, and `clock` controls, `docs/AUDIOFLEX.md`, an isolated timeline/math test and a source ownership audit.
- `ActivateAudioInterfaceAsync` is intentionally deferred because safe observation requires completion-handler-aware interception rather than treating asynchronous activation like the MMDevice path.

## R4.2 - XRFlex canonical clock

- Migrated the common graphics timing helper from wall-clock FILETIME to monotonic QueryPerformanceCounter nanoseconds.
- Added OpenXR `xrCreateSession` interception for session-to-instance clock capability binding.
- Added standards-based per-timepoint `XrTime` -> QPC conversion through `XR_KHR_win32_convert_performance_counter_time` when enabled by the application/runtime.
- Added canonical XR predicted/submitted display timestamps and signed deadline slack to `XrFrameToken`.
- No cached XrTime offset, no extension injection, no new sleep/pacing owner.
- Added canonical-clock unit test and architecture audit.

## R4.0 - Adaptive Scheduler / Structural-Stall Awareness

## 0.9.0-vulkanflex-r4.0.1

- Add a lock-free process-lifetime transport-truth plane that separates weak Vulkan hook-chain heuristics from directly proven application transport.
- Treat Wine D3D/Vulkan module coexistence as `TranslationBypassSuspected` only; it can suppress risky Vulkan detours but cannot by itself classify the application as VKD3D/DXVK.
- Confirm Native Vulkan only from the intercepted/tracked `VkDevice` path, VKD3D from `ID3D12DXVKInteropDevice`, and DXVK from `IDXGIVkInteropDevice`. Conflicting strong evidence becomes `Mixed` instead of erasing a real route.
- Add `native-vulkan-explicit` execution mode when a weak translation heuristic caused WSI-hook bypass: explicit frontend pacing stays available, while core WSI pacing, Present Precision, Queue Pressure and Structural-Stall shielding are disabled.
- Shadow Vulkan frontend callbacks behind an already-active, directly confirmed VKD3D/DXVK route so they cannot switch/deinitialize the D3D pacing owner or add a second sleep.
- Add runtime diagnostics distinguishing suspected hook ownership from confirmed transport identity.
- Add `transport_truth_test.cpp` and `audit-authoritative-transport-truth.sh`.

- Added passive timing interception for native-Vulkan `vkCreateGraphicsPipelines` and `vkCreateComputePipelines` lookups. Calls are forwarded unchanged; VulkanFlex only records long-call duration as structural evidence.
- Added a fixed-size, epoch-tagged structural-stall evidence plane with no heap allocation or blocking on pipeline/compiler threads.
- Added a robust completion-gap baseline and conservative classifier: a large frametime alone is never sufficient; a recent long pipeline-creation call must corroborate the outlier.
- Added `LatencyFleX::CompensateExternalStall()` to collapse only the confirmed external pause interval while preserving estimator history and the real clock domain.
- Structural shielding clears pending soft queue-pressure delay for the affected recovery point but does not create a second sleep or pacing owner.
- Added `[vulkanflex] structural_stall`, `pipeline_threshold_us`, and `stall_max_compensation_us`.
- Added host tests for the classifier and LatencyFleX time-base compensation plus a source audit that rejects command-buffer/queue/pipeline rewriting from the classifier.

## R3.9 - Transport-neutral Vulkan Capability Engine

- Adds a passive `VulkanCapabilitySnapshot` shared by native Vulkan, VKD3D and DXVK transport discovery.
- Separates physical-device `supported`, concrete-device `enabled`, and `enabled_known` state so translation bridges never turn unknown enablement into a false positive.
- Models Vulkan 1.3 and 1.4 explicitly, including timeline semaphore, synchronization2, dynamic rendering/local-read, maintenance5/6 and pipeline robustness.
- Models EDS1 and the Vulkan-1.3 core portion of EDS2 separately from the still-optional EDS2 logic-op and patch-control-points features.
- Models `VK_EXT_extended_dynamic_state3` as a capability family plus all 31 individual feature bits; no single `eds3=true` value is used as a substitute for feature granularity.
- Adds descriptor-buffer, graphics-pipeline-library, shader-module-identifier, unified-image-layouts, calibrated-timestamp and present-precision capability state.
- Native Vulkan merges the exact `VkDeviceCreateInfo`; VKD3D merges public interop enabled-extension/feature state; DXVK remains support-only where its public ABI cannot prove device enablement.
- Publishes the snapshot through `VulkanDeviceRegistry` and exposes it to VulkanFlex translation observers without changing pacing ownership.
- Capability discovery is control-plane only: no pipeline rewriting, command-buffer mutation, queue submission, extension injection or swapchain mutation is introduced.
- Adds strict host unit coverage and an architecture audit for version promotion, EDS2/EDS3 granularity, unknown-state preservation and no-mutation invariants.

## R3.8 - Frame Generation Dual Timeline

- Splits HybridFusion's canonical identity into an existing monotonic render timeline plus a new independent monotonic presentation timeline.
- Adds fixed-size `CanonicalPresentationToken` records carrying `render_sequence`, `present sequence`, per-render `generation`, interpolation state, source frontend, transport and source-domain frame ID.
- Models frame generation as a 1:N render-to-presentation relation instead of assuming `RenderFrame == PresentFrame`; no fixed 2x/3x FG multiplier is encoded.
- Routes `Fake_InformPresentFG` through the Reflex frontend/HybridFusion path instead of bypassing arbitration with a direct backend `set_fg_type()` call.
- Adds an optional `LowLatencyTech::observe_canonical_presentation()` ABI so VKD3D/DXVK VulkanFlex observers can consume FG mapping without becoming pacing owners.
- Keeps generated-presentation handling telemetry-only: no extra sleep, LatencySleep call, Vulkan submit, present wait, or completion feedback is generated by the FG path.
- Uses 128 fixed presentation slots, monotonic sequence allocation, per-render generation counters, a fail-open publication gate and epoch invalidation; no heap container enters the marker/FG hot path.
- Adds isolated dual-timeline unit coverage and an architecture audit enforcing OptiScaler routing, 1:N mapping and no-second-pacer invariants.

## R3.7 - Soft Queue Pressure Governor

- Adds a native-Vulkan/executor-only queue-pressure governor on top of the R3.6 precision-completion layer.
- Derives presentation backlog from fixed pending present-ID slots and tracks CPU-ahead as a secondary diagnostic/amplifier; no `vkQueueSubmit*` interception is added.
- `queue_pressure=auto` arms only after at least eight precise VF2+ completions and a <=25% CPU-present fallback ratio; uncertain completion evidence fails open.
- Applies pressure as a bounded adjustment to the existing VulkanFlex/LatencyFleX wake target instead of introducing a second sleep owner. The default extra-delay ceiling is 2000 us.
- Keeps `vkWaitForPresentKHR` / `vkWaitForPresent2KHR` at timeout 0. R3.7 therefore avoids relying on implementation-dependent blocking timeout granularity.
- Restricts the governor to `Role::Executor + NativeVulkan`; VKD3D observer/XeLL and DXVK delegate/observer ownership remain unchanged.
- Makes queue-pressure evidence epoch-aware so stale pre-reset presents cannot arm or pressure a new Off->On session.
- Adds `queue_pressure`, `queue_target_presents`, and `queue_max_delay_us` policy/config controls plus fixed aggregate diagnostics.
- Adds isolated queue-governor unit coverage and an architecture audit enforcing no-submit/no-blocking/single-owner invariants.

## R3.6 - Passive Vulkan present precision ladder

- Adds a capability-gated VF0→VF4 precision ladder: core WSI, present ID, `VK_KHR_present_wait`, `VK_KHR_present_wait2`, and `VK_EXT_present_timing`.
- Observes only extensions/features/swapchain flags/present metadata already enabled by the application; R3.6 never enables present-precision extensions or mutates swapchain flags.
- Hooks `vkCreateSwapchainKHR` only to retain existing swapchain flags and resolves `vkWaitForPresentKHR`, `vkWaitForPresent2KHR`, and `vkGetPastPresentationTimingEXT` from the existing device dispatch.
- Uses present-wait APIs with timeout 0 as completion sensors only. Blocking queue-depth control remains reserved for R3.7.
- Defers CPU-side `vkQueuePresentKHR` completion feedback when stronger evidence is available, with bounded fixed-storage fallback so the estimator cannot starve.
- Keeps VKD3D/DXVK ownership unchanged: present precision is executable only for the native-Vulkan VulkanFlex executor in R3.6.
- Extends the VKD3D capability snapshot with present-id/wait/wait2/timing extension and feature granularity for future cooperative telemetry.
- Adds unit and architecture regression coverage for the precision selector and passive/no-submit invariants.

## R3.5 - DXVK cooperative bridge and pacing ownership

- Adds direct DXVK `IDXGIVkInteropDevice` detection and Vulkan device/queue identity.
- Adds DXVK `ID3DLowLatencyDevice` delegation so DXVK can remain the sole pacing owner.
- Adds safe `dxvk_execution=auto|0|1` policy; Auto never starts an independent VulkanFlex wait when DXVK lacks its native LL interface.
- Extends D3D11 routing and canonical FrameToken transport classification to `DxvkD3D11`.
- Keeps translation-stack Vulkan detours, queue locks and Vulkan submissions disabled.

## 0.9.0-vulkanflex-r3


## VulkanFlex R3.1 build portability fix

- Fix GCC/MinGW compilation of the primary-swapchain CAS by giving the `expected` value the explicit `VkSwapchainKHR` type instead of deducing `std::nullptr_t` from `VK_NULL_HANDLE`.
- Harden the fixed-MPMC stress test against a legitimate transient empty observation that can occur immediately before the final producer publication becomes synchronized with the consumer.
- Extend the GCC/MinGW source audit to reject the same untyped `VK_NULL_HANDLE` CAS pattern in future changes.

- Add `VulkanFlex`, a Vulkan-exclusive cross-vendor output backend derived from the embedded LatencyFleX scheduler.
- Add portable core-WSI pacing/feedback interception for `vkAcquireNextImageKHR`, `vkAcquireNextImage2KHR`, `vkQueuePresentKHR` and swapchain destruction.
- Keep one pacing owner: core WSI drives pacing only when no explicit frontend owns pacing; explicit per-frame Sleep suppresses marker-triggered duplicate sleeps.
- Extend startup-locked HybridFusion to native Vulkan execution so Reflex, `VK_NV_low_latency2`, XeLL and Anti-Lag 2 can contribute the best frozen source per semantic aspect while VulkanFlex remains the sole executor.
- Add fixed-capacity swapchain/image-index maps and a lock-free MPMC present-feedback ring; no per-frame heap allocation and no blocking scheduler mutex.
- Elect exactly one primary swapchain for core pacing/completion feedback so auxiliary windows cannot double-pace or close the main frame sample early.
- Add Off→On epoch tagging and estimator reset fencing so stale presents from the prior control epoch are discarded.
- Preserve native `VK_NV_low_latency2` ownership in Auto when native extensions are preferred; explicit `vulkanflex` output can override it.
- Add `[vulkanflex]` policy and `vulkan_fusion` hybrid control.
- Treat core WSI feedback honestly as CPU-side acquire/present timing, not physical display completion; higher-fidelity present-wait/timing integration remains a separate capability tier.

## 0.9.0-low-level-vkd3d-capability-hardening

- Centralize D3D12 -> vkd3d-proton -> Vulkan capability probing so output recognition and backend initialization consume the same device truth.
- Recognize the current optional `ID3D12DXVKInteropDevice2` IID while keeping execution on the stable base interop ABI.
- Mirror the current base interop method order through queue/resource/queue-lock methods, including `GetVulkanQueueInfo`.
- Capture the borrowed Vulkan device's API/vendor/device/driver identity and queue-family topology without creating a second Vulkan device.
- Recognize enabled low-level primitives including `VK_AMD_anti_lag`, `VK_NV_low_latency2`, timeline semaphores, synchronization2, present-id, descriptor buffers, device-fault reporting and calibrated timestamps.
- Treat timeline semaphores (Vulkan 1.2) and synchronization2 (Vulkan 1.3) as core capabilities when the extension name is absent after promotion.
- Remove the global registry mutex from the steady-state native `vkQueueNotifyOutOfBandNV` lookup by reusing the existing TLS queue/device dispatch caches.

## 0.9.0-io-capability-recognizer

- Replace marker-only input presence inference with fixed-size multi-evidence recognition: capability query, frontend context, control packet, sleep cadence, marker traffic, async markers and frame-generation metadata are tracked independently.
- Keep passive capability/context discovery non-driving: a frontend must still produce fresh frame activity before it can win input arbitration.
- Fold one-time evidence bonuses into the existing monotonicity/diversity quality score so real API use reaches confidence faster than merely exposed UI capability.
- Add a tri-state output recognizer (`available` / `unavailable` / `unknown`) so only proven-negative backends are filtered; uncertain backends remain safe fallbacks.
- Keep explicit output policy authoritative: if a forced backend is proven unavailable and fallback is disabled, return no candidate instead of silently substituting another backend.
- Add D3D12 output probing for real vkd3d-proton device interop, enabled `VK_AMD_anti_lag` extension/feature, `IAmdExtAntiLagApi`, loaded/present XeLL, and the software LatencyFlex fallback.
- Add D3D11 output probing for Anti-Lag 2 runtime presence while preserving LatencyFlex as the guaranteed software fallback.
- Add Vulkan output recognition from the per-device registry instead of relying only on static backend ordering.
- Log frozen input evidence/marker masks and output capability confidence at startup; perform no DLL search, COM probing or extension traversal on the frame hot path.
- Reset input recognition and selected pacing ownership on backend teardown so a new backend session starts a clean discovery epoch.
- Add `test-io-recognizer.sh` and `audit-io-recognizer.sh`.

## 0.8.0-low-level-execution

- Add a capability-gated D3D12 -> vkd3d-proton -> `VK_AMD_anti_lag` execution backend using the existing vkd3d Vulkan device rather than the AMD Windows wrapper hot path.
- Mirror only the minimal public `ID3D12DXVKInteropDevice` ABI required for extension/feature/device-handle discovery; add no vkd3d link-time dependency.
- Require both `VK_AMD_anti_lag` in the enabled device-extension list and `VkPhysicalDeviceAntiLagFeaturesAMD::antiLag == VK_TRUE` before activating the direct path.
- Resolve `vkGetDeviceProcAddr` / `vkAntiLagUpdateAMD` once during initialization; never create a Vulkan device or install another Vulkan-core detour chain.
- Calibrate exact INPUT/PRESENT frame pairing before enabling presentation-stage metadata; until then use the valid base Anti-Lag update form without a synthetic frame mapping.
- Prefer direct AMD Vulkan execution only when D3D12 output is Auto and `[vulkan] prefer_native_extensions=1`; disabling native preference fully removes this backend from the D3D12 candidate list.
- Establish the Hybrid execution backend before startup evidence collection so the first marker/Sleep cannot accidentally use the legacy selector path.
- Add an atomic unchanged-route fast path that avoids `active_tech_mutex` on steady-state D3D frontend calls.
- Stop rerunning Hybrid input selection after the startup ownership map is frozen.
- Apply effective-FG and enable-override backend state only on actual changes.
- Compile frame-hot trace diagnostics out of release builds through `VFN_HOT_TRACE`.
- Suppress repeated empty `GetLatency()` diagnostics after the first occurrence per backend session.
- Replace floating-point interval-to-FPS conversion with integer arithmetic in the direct and native AMD Vulkan paths and the Anti-Lag 2 fallback sleep path.
- Keep direct D3D12 `VK_NV_low_latency2` execution disabled until a real `VkSwapchainKHR` can be obtained safely; retain existing native/vkd3d-managed LL2 behavior and XeLL fallback.

## 0.7.0-startup-locked-hybrid-xell-r2
### r3 coexistence classification

- Classify an already-loaded Vulkan chain before installing Vulkan core detours.
- Treat game-local `dxgi.dll`, `d3d11.dll`, `d3d12.dll`, or `OptiScaler.dll` as an OptiScaler/game-proxy-owned chain.
- Under Wine only, classify D3D12 + an existing Vulkan loader as vkd3d-proton-like and D3D11 + an existing Vulkan loader as DXVK-like.
- Native Windows does not infer vkd3d/DXVK merely because `vulkan-1.dll` is loaded, avoiding overlay-induced false positives.
- The classification is diagnostic/guard-only; Reflex, XeLL and Anti-Lag 2 frontends remain available and explicit Vulkan force settings still override the guard.


- Repair MinGW/GCC target-build failures found by the first real CachyOS build of r1.
- Split the runtime-selected Hybrid XeLL log message into compile-time fmt literals, satisfying fmt 12 consteval format checking.
- Move packed frontend sleep-mode inspection behind a `LowLatency` member helper so `low_latency_d3d.cpp` no longer references translation-unit-local helpers/constants from `low_latency.cpp`.
- Explicitly discard the metadata-only `publish_fg_type()` return value to keep `[[nodiscard]]` diagnostics clean.
- No routing, scoring, startup-lock, frame-ID, output-backend, or protocol semantic changes.

## 0.7.0-startup-locked-hybrid-xell-r1

- Correct 0.7 semantics: per-aspect source selection happens once during startup, not adaptively during gameplay.
- Add an observation-count warmup (`startup_observations`, default 64) with no provisional XeLL pacing.
- Freeze the complete per-aspect ownership map atomically for the backend session; owners are reset only on backend teardown.
- Keep game-originated XeLL as a first-class startup candidate.
- Preserve each non-pacing frontend's own reported enabled state in Hybrid `GetSleepStatus()` so fixed secondary aspect sources keep emitting metadata without gaining pacing authority.
- Route Hybrid XeLL sleep with the exact frame ID of the locked Pacing owner.
- Disable XeLL's legacy `simulation_start + 1` / marker-triggered fallback sleep once exact-frame hybrid pacing is active.
- Preserve one output / one pacing mechanism.

## 0.7.0-aspect-adaptive-hybrid-xell

- Replace global primary/secondary Hybrid XeLL fusion with independent per-aspect arbitration.
- Add aspect owners for pacing, enabled, Boost, minimum interval, marker optimization, simulation/render/present lifecycle, input sampling, out-of-band lifecycle and frame-generation metadata.
- Treat game-originated XeLL as a first-class input source; keep the separate XeLL output context observation-free.
- Keep exactly one pacing/Sleep owner even when Reflex + Anti-Lag 2 + XeLL coexist.
- Add per-aspect semantic fitness, freshness/quality threshold and switch hysteresis.
- Preserve exact numeric frame correlation and duplicate suppression for cross-source markers.
- Add host regression coverage proving AL2 pacing + Reflex Boost + XeLL interval selection in one canonical XeLL output state.


## 0.6.0-hybrid-xell-r1

- Fixed canonical hybrid control resolution so fields not owned by the primary frontend start from neutral values instead of leaking implementation-local bits.
- Prevents Anti-Lag 2's local `use_markers_to_optimize=true` from appearing as `markers=true(auto)` when AL2 does not semantically provide that field.
- Secondary filling remains strict fill-missing; explicit primary values still win.
## 0.6.0-hybrid-xell

- Add a Hybrid Low-Latency fusion layer specifically for the XeLL D3D12 output.
- Keep exactly one pacing owner: only the quality-selected primary frontend may call the backend `Sleep()`.
- Allow fresh secondary frontends to fill control fields the primary protocol does not define; e.g. Reflex Boost may enrich an Anti-Lag 2 primary without overriding the primary enable state.
- Fuse secondary markers only into an already established primary frame and only when the numeric frame ID matches exactly; no heuristic +/-1 frame remapping.
- Preserve secondary Anti-Lag 2 frame-generation annotations in the canonical frame state; XeLL currently has no direct FG-type setter, so these remain metadata until a safe output mapping exists.
- Prefer XeLL first when D3D12 output is `auto` and `[hybrid] xell_fusion=1`; explicit output selections remain authoritative.
- Add `[hybrid] xell_fusion`, `fill_missing_signals`, and `secondary_minimum_quality`.
- Add native host regression test and architecture audit for Hybrid XeLL.

## 0.4.0-vulkan-input-r6

- Exclude frontends that explicitly report low-latency OFF from input arbitration while continuing to observe their markers for telemetry/freshness.
- Keep a healthy incumbent sticky on equal quality; configured priority is now only an initial/no-incumbent tie-breaker.
- Preserve r5 lock-free CAS arbitration and r4 LatencyFlex OFF→ON lifecycle reset.
- This specifically addresses Ghost of Tsushima exposing AL2 while disabled/greyed Reflex/XeLL markers continue to arrive.

# Changelog

## 0.4.0-vulkan-input-r5

- Fix concurrent input-arbiter commit race seen in Ghost of Tsushima when Reflex and Anti-Lag 2 markers arrive from different frontend threads.
- Replace stale unconditional `selected_input.store()` with lock-free compare/exchange arbitration and re-evaluation against the newly committed incumbent on contention.
- Prevent Reflex/Anti-Lag 2 input flapping from repeatedly resetting frontend sleep-mode application and desynchronizing the single XeLL output backend frame cadence.
- Add `audit-input-arbitration.sh` regression audit.

## 0.4.0-vulkan-input-r4

- Fix LatencyFlex lifecycle when an application keeps emitting Reflex markers while in-game Reflex is disabled.
- Suppress LatencyFlex `EndFrame()` while low-latency mode is disabled so it cannot accumulate unmatched EndFrame calls without BeginFrame.
- Reset the LatencyFlex epoch on an in-game Reflex Off -> On transition before the first active BeginFrame.
- Add `audit-latencyflex-lifecycle.sh` regression audit.


## 0.4.0-vulkan-input

- Add `VK_NV_low_latency2` as a real Vulkan input frontend.
- Intercept Vulkan device-extension enumeration and expose/hide `VK_NV_low_latency2` according to `[vulkan]` policy.
- Intercept `vkGetDeviceProcAddr` and `vkGetInstanceProcAddr` and provide the five low-latency2 entry points.
- Translate `vkSetLatencySleepModeNV`, `vkLatencySleepNV` and `vkSetLatencyMarkerNV` into the shared Vulkan low-latency bridge on non-native drivers.
- Preserve the Vulkan contract that `vkLatencySleepNV` returns immediately: emulated pacing runs on a fixed-capacity asynchronous worker and signals the application-provided timeline semaphore after pacing completes.
- Gate cross-vendor exposure on the official low-latency2 dependencies: Vulkan 1.2 or `VK_KHR_timeline_semaphore`, plus `VK_KHR_present_id` or `VK_KHR_present_id2`.
- Reject `VK_NV_low_latency2` device creation for multi-device device groups, which the extension does not support.
- Treat `vkSetLatencySleepModeNV(..., nullptr)` as disable/reset instead of an invalid call.
- Emulate `vkGetLatencyTimingsNV` from the existing frame-report ring.
- Treat `vkQueueNotifyOutOfBandNV` as control-plane registration; native drivers receive pass-through while emulated backends consume explicit OOB markers.
- Preserve native NVIDIA `VK_NV_low_latency2` when `prefer_native_extensions=1`; native marker/sleep calls are still observed by the common input arbiter so only one input can pace.
- Strip an emulated NVIDIA extension from the real `VkDeviceCreateInfo` before calling a non-NVIDIA driver, without mutating caller-owned structures.
- Extend the per-device Vulkan registry with native low-latency2 dispatch and queue-to-device tracking.
- Add delayed `vulkan-1.dll` discovery through LoadLibrary detours, removing the previous startup-order limitation.
- Add Vulkan low-latency2 architecture auditing and extend the Vulkan registry regression test.

## 0.3.0-inputs-r1

- Fix the MinGW compile boundary for the Anti-Lag 2 DX12 input proxy.
- Include `<d3d12.h>` before AMD `ffx_antilag2_dx12.h`, which references `ID3D12Device` without declaring it itself.
- Preserve the 0.3.0 proxy ABI, arbitration logic and runtime behavior unchanged.
- Extend the frontend audit to reject regressions in this required include ordering.

## 0.3.0-inputs

- Add a shared D3D frontend bridge enforcing `OBSERVE MANY -> SELECT ONE -> DRIVE ONE`.
- Remove XeLL's separate `LowLatency` output ownership so Reflex, XeLL and Anti-Lag 2 share one active backend.
- Add a synthetic game-facing XeLL D3D12 context and translate XeLL sleep/mode/marker calls into the common input model.
- Preserve real XeLL output operation by forwarding calls originating from the Vulkanized-Fakenvapi XeLL backend through the Detours trampoline.
- Replace native Anti-Lag 2 game suppression with D3D12 `IAmdExtAntiLagApi` and D3D11 `AmdDxExtCreate11` input proxies.
- Translate Anti-Lag 2 per-frame input timing and end-of-render signals into normalized markers.
- Preserve the real AMD driver interface exclusively for the selected Anti-Lag 2 output backend through explicit internal pass-through.
- Add observation-sequence freshness so disappeared frontends become stale without timer/syscall work in the marker hot path.
- Add quality-margin hysteresis with configured-priority tie breaking.
- Preserve per-frontend sleep-mode state and apply it only when that frontend is selected.
- Add universal-input architecture audits and multi-input regression tests.
- Keep native `VK_NV_low_latency2` interception out of scope for this milestone.

## 0.2.0-policy

- Freeze `0.1.0-foundation-r8` as the verified GCC/MinGW build baseline.
- Add named `[general]`, `[input]`, `[output]`, `[backend_order]`, `[vulkan]` and `[overlay]` policy sections.
- Preserve legacy 1.x configuration keys and translate `force_latencyflex` into the new output policy when no explicit new output is present.
- Replace mutable config fields on the runtime path with immutable `RuntimePolicySnapshot` objects published through one atomic pointer load.
- Keep published snapshots alive for process lifetime so hot-path readers require no locks or shared-pointer refcounts.
- Add fixed-capacity input/backend order containers.
- Add normalized common marker types and a fixed-size input-quality arbiter.
- Feed NVAPI Reflex latency markers into the common input model.
- Add configurable quality threshold and priority tie-breaking for input selection.
- Centralize D3D11, D3D12 and Vulkan backend candidate generation.
- Preserve the r8 default backend preference order while allowing explicit per-API output selection and fallback control.
- Add routing signatures so logging-only config reloads do not recreate the active backend.
- Preserve the Anti-Lag 2 delayed-deinit grace period used by OptiScaler/FSR-FG integrations.
- Initialize previously implicit `LowLatency` state (`currently_active_tech`, FG state) explicitly.
- Add host-native policy-engine tests and a hot-path architecture audit.

## 0.1.0-foundation-r4

- Removed NVAPI SAL annotations from Vulkan function definitions in project code.
  The MinGW wrapper intentionally scopes SAL macros to `nvapi.h` only.
- Fixed `fakexell::Init()` to return the Detours transaction result instead of
  falling off the end of a non-void function.
- Extended the GCC/MinGW source audit to reject leaked `__in`/`__out`/`__inout`
  annotations in `src/`.

## 0.1.0-foundation

- Renamed the project to Vulkanized-Fakenvapi while retaining NVAPI DLL names.
- Switched the language standard to C++23.
- Kept GCC / MinGW-w64 as the primary toolchain.
- Replaced the DXGI Meson compiler library probe with an explicit dependency.
- Added a CI/audit guard against `find_library()` and `has_library()` probes.
- Pinned the supplied Vulkan-Headers 1.4.362 and spdlog 1.17.0 revisions.
- Added per-device Vulkan state and device-function dispatch.
- Added per-device NVAPI Vulkan timeline semaphore ownership.
- Added `vkDestroyDevice` cleanup.
- Stopped modifying caller-owned `VkDeviceCreateInfo` structures.
- Added explicit `VK_AMD_anti_lag` extension/feature discovery.
- Prevented duplicate extension injection.
- Skip Vulkan Anti-Lag injection for multi-device groups.
- Added architecture and roadmap documents.

### Packaging fix r1
- Restored the original GCC/MinGW cross files `build-win64.txt` and `build-win32.txt` that were accidentally omitted from the first foundation archive.

### GCC/MinGW compatibility repair r2
- Added `nvapi_mingw_compat.h` and routed project NVAPI includes through it.
- Enabled `__NVAPI_EMPTY_SAL` under MinGW so nested NVAPI lite headers do not remove SAL macros mid-parse.
- Neutralized MinGW's incompatible `__success` macro while parsing `nvapi.h`.
- Fixed both legacy Detours `_MSC_VER < 1299` checks for non-MSVC compilers.
- Replaced `_ReturnAddress()` with GCC's `__builtin_return_address(0)`.
- Added explicit Detours function-pointer casts required by strict GCC C++.
- Fixed the DXGI vtable restore function-pointer conversion.
- Added missing `<iomanip>` for `std::setprecision`.
- Added `scripts/audit-gcc-mingw-compat.sh`.

### foundation-r3
- Fixed MinGW NVAPI SAL cleanup so `__in`, `__out`, and related annotation macros cannot leak into libstdc++ headers.
- Kept NVIDIA SAL annotations alive only while nested `nvapi_lite_*` headers are parsed, followed by one explicit `nvapi_lite_salend.h` cleanup pass at the wrapper boundary.
- Extended the GCC/MinGW compatibility audit to guard against SAL macro leakage regressions.

## 0.1.0-foundation-r5

- Stop linking the prebuilt MSVC `external/detours/lib/detours.lib` into GCC/MinGW builds.
- Pin Microsoft Detours source at commit `adb07604aa56508448b95bf037c2a6d0d3b6831a`.
- Build only the Detours core files required by Vulkanized-Fakenvapi (`detours.cpp`, `disasm.cpp`) as a Meson static subproject using the same MinGW compiler as the DLL.
- Add a build audit that rejects accidental reintroduction of the MSVC Detours library.

## Foundation r6

- Fixed the Detours Meson subproject target preflight.
- Removed the Detours subproject machine hard-fail entirely.
- The parent cross build selects the MinGW host compiler and the normal subproject target inherits it; Windows headers provide the authoritative compile-time validation.

## 0.1.0-foundation-r7

- Diagnose accidental native Linux Meson configuration at the root project.
- Restore the Detours Windows-host guard with a precise parent-toolchain error.
- Document that a Meson build directory must be recreated when switching from
  native to MinGW cross compilation.
- No runtime/DSP/low-latency behavior change.

## 0.1.0-foundation-r8

- Complete the minimal GCC/MinGW Detours source set by adding `src/modules.cpp`.
- Fix the final unresolved `DetourGetModuleSize` symbol emitted from Detours `disasm.cpp`.
- Keep the Detours source build minimal: `detours.cpp`, `disasm.cpp`, and `modules.cpp` only.
- Extend the Detours toolchain audit to require `modules.cpp`.
- No runtime low-latency policy change.
### 0.4.0-vulkan-input-r2

- In Vulkan auto-coexistence mode, the deprecated `NvAPI_Vulkan_InitLowLatencyDevice` path now returns `NVAPI_NOT_SUPPORTED` for intentionally unregistered external Vulkan devices instead of attempting to create a timeline semaphore and emitting `VK_ERROR_INITIALIZATION_FAILED`.
- Reflex/XeLL/Anti-Lag2 input arbitration and the selected LatencyFlex output path are unchanged.


### 0.4.0-vulkan-input-r3
- Restores the r1-compatible `NVAPI_ERROR` result for deprecated Vulkan low-latency initialization when auto-coexistence leaves the foreign `VkDevice` unregistered.
- Keeps the r2 safety improvement: no timeline semaphore is created for an unowned device, so the `VK_ERROR_INITIALIZATION_FAILED (-3)` log remains eliminated.
- No changes to the Reflex/XeLL/Anti-Lag2 arbiter, LatencyFlex backend selection, or Vulkan coexistence guard.

## 0.5.0-capability-emulation

- Added game-facing Streamline Reflex capability emulation without coupling the
  exposed frontend to the selected low-latency output backend.
- `slIsFeatureSupported(kFeatureReflex)` is exposed as supported and
  `slReflexGetState()` reports `lowLatencyAvailable=true` to unlock the host
  application's Reflex UI path on cross-vendor systems.
- Expanded XeLL synthetic-context interception from EXE-only callers to ordinary
  game-owned modules beside the executable, covering engines such as Nixxes
  which create XeLL contexts from a game DLL.
- Infrastructure DLLs (fakenvapi, OptiScaler proxies, Streamline and libxell)
  are excluded from the synthetic game-context path so output XeLL remains a
  separate real backend.
- Preserves the architecture invariant: exposed frontend capability and selected
  output backend are independent.


## 0.5.1-antilag2-output

- Prefer vkd3d-proton's D3D12 `IAmdExtAntiLagApi` implementation as the Anti-Lag 2 output on Wine/Linux.
- This lets vkd3d-proton translate the AMD Anti-Lag 2 ABI to `VK_AMD_anti_lag` when the Vulkan driver/layer exposes it.
- Preserve the native Windows `amdxc64.dll` path as fallback.
- Preserve XeLL and LatencyFlex as subsequent policy fallbacks.

## 0.9.0-vulkanflex-r3.2 — transport ownership hardening

- Treat `vulkanflex.only_native_vulkan=1` as an early transport-ownership rule on Wine D3D→Vulkan stacks, not only as a pacing-time filter.
- Detect probable vkd3d-proton / DXVK transport before installing Vulkan core or WSI detours and bypass those hooks completely in native-only mode.
- Preserve D3D, Reflex, XeLL and Anti-Lag 2 frontends while the translation layer retains ownership of its Vulkan dispatch chain.
- Revalidate delayed `vulkan-1.dll` loads through `VulkanHooks::initialize()` instead of calling `hook_vulkan()` directly, closing a policy-bypass path during late startup.
- Add `audit-vulkan-transport-ownership.sh` to lock the transport contract.

## 0.9.0-vulkanflex-r3.3 — cooperative VKD3D bridge

- Add VulkanFlex as a capability-gated D3D12 backend when the concrete device exposes vkd3d-proton `ID3D12DXVKInteropDevice`.
- Preserve the R3.2 translation-stack Vulkan-detour bypass; communication now occurs through the public COM interop ABI instead of stacked WSI hooks.
- Borrow VKD3D Vulkan device identity and bind concrete D3D12 command queues to their real `VkQueue` / queue-family with `GetVulkanQueueInfo`.
- Route D3D12 async/OOB queue identity to the active backend before semantic Hybrid filtering, so transport discovery is not lost when an OOB marker aspect is not selected.
- Make VulkanFlex a first-class D3D12 startup-locked Hybrid executor. Reflex/XeLL/Anti-Lag2 can independently provide pacing/control/marker aspects while VulkanFlex remains the sole scheduler.
- Close LatencyFleX feedback from D3D12 `PRESENT_END` / OOB present-end exactly once per render frame.
- Add `vulkanflex.vkd3d_bridge=1` (default on) and include it in the routing signature.
- Keep the bridge non-invasive: no Vulkan submissions, no interop command buffers, no VKD3D queue locks, and no translation-stack WSI detours.
- Add `audit-vkd3d-vulkanflex-bridge.sh`.

## 0.9.0-vulkanflex-r3.4 — D3D12 executor arbitration + Canonical FrameToken

- Restore XeLL as the preferred software D3D12 executor when a VKD3D bridge is available; keep VulkanFlex as explicit/fallback D3D12 execution rather than promoting it above XeLL.
- Prefer native D3D12 Anti-Lag 2 ahead of the generic VulkanFlex D3D12 fallback after XeLL.
- Add VulkanFlex `Observer` role for cooperative VKD3D transport telemetry behind XeLL/Anti-Lag2/LatencyFlex executors. Observer mode never sleeps or submits Vulkan work.
- Keep the observer in fixed `LowLatency` storage and publish it atomically; no observer `new/delete` lifecycle is added.
- Add transport-neutral Canonical FrameTokens with session epoch, monotonic sequence, transport tag and fixed O(1) per-frontend source-ID bindings.
- Translate pacing Sleep and accepted Hybrid markers to the same internal FrameToken before dispatching them to XeLL/VulkanFlex. Raw Reflex, XeLL and Anti-Lag 2 frame IDs are no longer assumed numerically equal.
- Reject previously unseen delayed source IDs rather than heuristically attaching them to the wrong token; already-bound delayed IDs still resolve in O(1).
- Harden XeLL lifecycle: exact slept-ID ring instead of sticky booleans, clear `libxell.dll` handle after unload, release the queried D3D12 device reference, and make sleep-mode deduplication context-local.
- Add `audit-vulkanflex-executor-arbitration.sh` and extend the Hybrid regression with deliberately different AL2/Reflex frame-ID domains.

## R3.4.1 - Canonical observer ABI fix

- Added the canonical timeline observer hooks to the `LowLatencyTech` base interface.
- `VulkanFlex::observe_canonical_frame_start()` and `observe_canonical_marker()` now validly override the polymorphic base methods.
- Fixes MinGW/GCC build failures where the R3.4 observer methods were declared `override` without matching virtual base members.
- No scheduler, FrameToken mapping, XeLL pacing, VKD3D transport, or hot-path algorithm changes.
