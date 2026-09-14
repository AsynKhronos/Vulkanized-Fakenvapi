# VulkanFlex R4.0.1

## Purpose

VulkanFlex is a cross-vendor low-latency execution backend with two safe transports: native Vulkan WSI and a cooperative D3D12/vkd3d-proton bridge. Native Vulkan games cannot rely on XeLL as a universal execution backend, while D3D12 games on Proton benefit from VKD3D's public Vulkan interop ABI without requiring a second Vulkan hook owner. VulkanFlex reuses the LatencyFleX pacing algorithm but integrates it into Vulkanized-Fakenvapi's startup-locked Hybrid architecture.

It is **not** a Vulkan extension, does not spoof a new driver feature, and does not require NVIDIA, AMD or Intel-specific low-latency support for its baseline path.

## Execution contract

```text
INPUT INFORMATION
  explicit: Reflex / VK_NV_low_latency2 / XeLL / Anti-Lag2
  fallback: core Vulkan WSI acquire + present
            |
            v
      startup arbitration
      one source per aspect
            |
            v
       VulkanFlex backend
            |
            v
  embedded LatencyFleX scheduler
```

There is always one execution backend and one pacing owner. The Hybrid layer is an information/provenance layer, not another scheduler. A vendor-specific Boost bit can be preserved as control evidence, but VulkanFlex does not invent a cross-vendor GPU-clock boost mechanism where Vulkan provides none.

### Baseline tier — Core WSI

A successful `vkAcquireNextImageKHR` or `vkAcquireNextImage2KHR` is used as the late frame-begin opportunity when no explicit pacing frontend is active. The generated frame ID is stored directly in the swapchain lane at the acquired `imageIndex`. Vulkan permits presentation order to differ from acquisition order, so exact image-index pairing is intentional rather than FIFO. `vkQueuePresentKHR` consumes that exact image slot and publishes CPU-side feedback to a fixed atomic slot. This avoids FIFO assumptions when multiple images are in flight. One swapchain is atomically elected as the primary pacing/completion owner; auxiliary swapchains never create a second sleep or close the primary frame sample.

This path requires no vendor extension. It applies to swapchain-based Vulkan rendering on any conformant implementation exposing the intercepted WSI entry points. Headless/offscreen rendering is outside this contract.

### Explicit frontend tier

If the game exposes Reflex-compatible NVAPI input, native/emulated `VK_NV_low_latency2`, XeLL or Anti-Lag 2 instrumentation, the existing input recognizer and HybridFusion score that evidence during startup. Pacing, Render, Present, Input Sampling and Control can each be frozen to the best source. VulkanFlex still remains the only execution backend.

If an explicit per-frame Sleep call is observed, Sleep becomes pacing owner and `SIMULATION_START` markers can no longer cause a second scheduler sleep. Off→On transitions start a new backend epoch; swapchain mappings and queued feedback are epoch-tagged so stale presents cannot contaminate the new LatencyFleX estimator state.

### Native-driver ownership

With `output.vulkan=auto` and `prefer_native_extensions=1`, a real native `VK_NV_low_latency2` path is not shadowed by VulkanFlex core pacing. This avoids two pacing systems fighting each other. Forcing `output.vulkan=vulkanflex` deliberately overrides that policy.

## Hot-path design

- no per-frame `new`, `delete`, `malloc` or dynamic container growth;
- embedded `lfx::LatencyFleX` state;
- fixed 16 swapchain lanes;
- fixed 64-image atomic frame map per swapchain lane;
- fixed 64-slot lock-free MPMC feedback ring;
- atomic packed control state;
- epoch-tagged acquire/present mappings for clean Off→On resets;
- one atomically elected primary swapchain so multi-window titles cannot double-pace;
- TLS swapchain-lane cache;
- `atomic_flag` try-gate around the single-writer scheduler; contention fails open rather than spinning or blocking;
- no second Vulkan device, queue or command-buffer owner.

## Accuracy boundary

Core WSI is a portability tier, not a physical-display telemetry tier. A return from `vkQueuePresentKHR` does **not** prove that the image has scanned out. Therefore R3 does not report or optimize against invented display-completion timestamps.

The natural higher-precision evolution is capability-gated use of standardized present ID / present wait / present timing mechanisms when they are actually enabled and safe for the surface. Those should improve feedback quality while leaving the VulkanFlex pacing-owner contract unchanged.

## Configuration

```ini
[output]
vulkan=auto

[backend_order]
vulkan=vulkanflex,amd_anti_lag,latencyflex

[hybrid]
vulkan_fusion=1
startup_locked=1
startup_observations=64
aspect_minimum_quality=60

[vulkanflex]
enabled=1
core_pacing=1
only_native_vulkan=1
vkd3d_bridge=1
minimum_interval_us=0
max_sleep_us=50000
```

Use `vulkan=vulkanflex` to force the backend for a targeted native Vulkan test. Keep `auto` for normal coexistence testing.

## R3.2 transport ownership rule

`only_native_vulkan=1` is now enforced before Vulkan detours are installed. On a probable Wine D3D→Vulkan translation stack (vkd3d-proton and/or DXVK), VulkanFlex does not own `vkGet*ProcAddr`, device creation, Acquire/Present, or swapchain teardown hooks. This is intentional: translation layers and graphics proxies may already interpose those dispatch paths, and stacking a second WSI owner during device bootstrap is unsafe.

The D3D-side semantic frontends remain available. A future explicit translation bridge can consume VKD3D/DXVK information cooperatively, but R3.2 does not pretend that bridge ownership is already safe.

## R3.3 cooperative VKD3D transport

The R3.2 translation-stack bypass remains authoritative for Vulkan detours. R3.3 adds communication with vkd3d-proton through `ID3D12DXVKInteropDevice` instead of re-entering VKD3D's Vulkan dispatch chain.

Control-plane initialization borrows VKD3D's existing Vulkan handles and records capability metadata. When a concrete `ID3D12CommandQueue` appears through Reflex OOB APIs, `GetVulkanQueueInfo` resolves its real `VkQueue` and queue-family index once and caches the identity. The bridge does not call `vkQueueSubmit`, create an interop command buffer, or take `LockCommandQueue` / `LockVulkanQueue`.

R3.4 changes the default D3D12 role after runtime A/B testing: XeLL is the preferred software executor and VulkanFlex normally becomes a cooperative VKD3D observer. The observer retains device/queue identity and canonical Present telemetry but never sleeps. VulkanFlex remains a D3D12 fallback/explicit executor when stronger D3D12 paths are unavailable or deliberately overridden.

## R3.4 Canonical FrameToken and observer role

HybridFusion no longer treats the pacing frontend's numeric ID as the canonical execution ID. It creates a monotonic internal token carrying `epoch`, `sequence`, `transport`, pacing-source identity and fixed source-ID bindings. Accepted markers are translated to `sequence` before reaching XeLL or VulkanFlex. This is required because Reflex, Anti-Lag 2, XeLL, Vulkan present IDs and future FG presents are independent numbering domains.

On D3D12/VKD3D Auto routing the order is intentionally executor-first: direct native Vulkan Anti-Lag when proven usable, XeLL, native D3D12 Anti-Lag 2, then VulkanFlex bridge fallback. When XeLL/AL2 owns execution, VulkanFlex can attach as `Role::Observer`; observer `sleep()` and `sleep_with_frame_id()` are no-ops.

## DXVK cooperative bridge (R3.5)

D3D11-on-DXVK is detected through DXVK's `IDXGIVkInteropDevice` COM ABI rather than by module-name heuristics. The bridge records the real Vulkan instance/device/submission queue without taking queue ownership. If DXVK exposes a supported `ID3DLowLatencyDevice`, `dxvk_execution=auto` keeps **DXVK as the sole pacing owner** and VulkanFlex delegates sleep-mode, sleep and canonical marker calls to it. If that interface is unavailable, Auto remains observer-only; `dxvk_execution=1` is required to permit the embedded VulkanFlex scheduler.

The DXVK bridge never calls `LockSubmissionQueue`, `ReleaseSubmissionQueue`, `vkQueueSubmit` or `vkQueueSubmit2`. Native-Vulkan WSI detours stay disabled on translation stacks.

## R3.6 present precision ladder

R3.6 upgrades the native-Vulkan completion signal without changing pacing ownership. `PresentPrecisionTier` selects: VF0 core acquire/present fallback, VF1 present-ID correlation, VF2 `VK_KHR_present_wait`, VF3 `VK_KHR_present_wait2`, or VF4 `VK_EXT_present_timing`. Selection is based only on device features/extensions already enabled by the application plus the actual swapchain creation flags and per-present metadata.

The layer never appends present-id/wait/timing extensions, never sets `VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR` or `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT`, and never invents a present ID. Wait calls use timeout 0. The fixed pending-completion slots retain the original CPU-present timestamp so unresolved precision probes fall back after a bounded number of acquire-side polls. Present-timing reports are treated as completion evidence; their timestamps are not mixed into VulkanFlex's separate scheduler clock domain. Translation-stack Vulkan detours remain bypassed, so VKD3D and DXVK continue to own their presentation paths.


## R3.8 frame-generation dual timeline

Frame generation is represented as two clock domains. The canonical render sequence remains the ID consumed by the single pacing executor. A separate canonical presentation sequence represents display events and stores a reverse link to the render sequence plus an unbounded-by-policy generation ordinal and an interpolated flag. This deliberately supports 1:N presentation without assuming a particular FG multiplier.

`Fake_InformPresentFG()` is now routed through the Reflex frontend into the startup-locked FrameGeneration aspect. The selected FG source alone can publish presentation tokens. Translation observers receive `PresentationParams { render_frame_id, present_frame_id, generation, interpolated }`; the callback is observer-only and is forbidden from entering any sleep/scheduler/completion-feedback path.

Storage is fixed: 128 presentation slots plus per-render atomic presentation counters. Epoch reset invalidates old presentation mappings. A publication gate fails open under pathological concurrent FG callbacks rather than blocking the render/present thread.

## R3.7 soft queue-pressure governor

R3.7 turns R3.6 completion evidence into bounded producer backpressure without introducing a new Vulkan synchronization owner. On the primary native-Vulkan swapchain, VulkanFlex counts still-pending precision slots after each acquire-side completion probe. `cpu_ahead = begun_frames - completion_signals` is retained as secondary context, but a CPU-ahead excursion alone is never enough to delay a frame.

Auto mode arms only when the best observed precision tier is VF2 or higher, at least eight precise completions have been seen in the current epoch, and CPU-present fallbacks remain at or below 25% of the completion sample. Once armed, backlog above `queue_target_presents` produces an adaptive delay of roughly one eighth of the observed frame period per pressure unit, capped by `queue_max_delay_us`. The delay is merged with the LatencyFleX target using `max(latencyflex_target, governor_target)` and is therefore still executed by the same single VulkanFlex sleep path.

R3.7 intentionally leaves `vkWaitForPresentKHR` and `vkWaitForPresent2KHR` at timeout 0. Vulkan allows timeout granularity to be implementation-dependent, so the default governor stays under VulkanFlex's own bounded wait instead of trusting a driver blocking timeout. No queue-submit hook, GPU fence, queue lock, command buffer, extension injection, or swapchain flag mutation is added.
## R3.9 transport-neutral capability model

`VulkanCapabilitySnapshot` is the common Vulkan control-plane description for native Vulkan, VKD3D and DXVK. Three masks are intentionally independent: `supported` describes physical-device support, `enabled` describes features known enabled on the concrete device, and `enabled_known` distinguishes a proven disabled feature from an enablement state the bridge cannot observe. EDS3 has parallel supported/enabled/known masks for every individual `VkPhysicalDeviceExtendedDynamicState3FeaturesEXT` bit.

Native Vulkan obtains exact enabled state from the unmodified `VkDeviceCreateInfo`. VKD3D can merge `GetDeviceExtensions` and `GetDeviceFeatures` from its public COM interop. DXVK public interop provides Vulkan identity but not its private device-create contract, so optional enabled-state remains unknown rather than guessed. The snapshot is telemetry/policy input only: it cannot enable an extension, modify a feature chain, rewrite a graphics pipeline, record a command, submit queue work or become a second pacing owner.

## R4.0 adaptive scheduler / structural stalls

R4.0 introduces an evidence-gated structural-stall shield for the native Vulkan executor. The Vulkan hook layer times `vkCreateGraphicsPipelines` and `vkCreateComputePipelines` calls but forwards all parameters and results unchanged. Long calls are published into eight fixed slots tagged with the active VulkanFlex epoch.

At completion-consumption time VulkanFlex compares the real completion gap with a slow EWMA clean-frame baseline. Auto mode requires eight baseline samples. A stall is classified only when the gap is at least 2x the expected elapsed-frame gap (and at least 4 ms above it) **and** recent pipeline-compilation evidence exists. The strongest recent pipeline event bounds the maximum removable interval, which is capped by `stall_max_compensation_us`.

`LatencyFleX::CompensateExternalStall()` collapses only the selected `[stall_start, stall_end]` interval in its internal reference timestamps. References after that interval are untouched. This preserves the scheduler's learned throughput/latency state and avoids a full estimator reset.

No structural-stall path may pace independently. Queue-pressure delay is cleared when a structural stall is shielded so stale backlog response does not compound the recovery frame.

## R4.0.1 authoritative transport truth

R4.0.1 fixes a runtime ambiguity where the hook installer could conservatively bypass a Wine D3D12+Vulkan stack while the Vulkan output recognizer later instantiated a full native-Vulkan executor. The new truth plane makes that state explicit. A weak bypass leaves the application transport unknown and yields `native-vulkan-explicit`: explicit frontend semantics can still drive the single scheduler, but all features that require authoritative WSI telemetry are disabled.

Direct VKD3D/DXVK interop confirmation blocks a separate Vulkan executor and causes Vulkan-shaped frontend callbacks to be shadowed while the matching D3D route is active. A genuinely intercepted native Vulkan device yields full WSI access. This preserves the single-owner rule without treating OptiScaler/helper modules as authoritative translation evidence.

## R4.3 domain separation

AudioFlex is intentionally not part of VulkanFlex execution. WASAPI padding/clock telemetry is owned by the audio domain and cannot alter VulkanFlex sleep, queue-pressure, present-precision or structural-stall state in R4.3. Cross-domain orchestration remains deferred until the audio clock/queue model is validated.

## R4.5 Opt5 — two-phase WSI correlation and bounded precision hot path

Opt5 tightens native-Vulkan WSI correlation without changing queue ownership. Before forwarding `vkQueuePresentKHR`, the hook atomically claims the current swapchain-image -> canonical-frame mapping into a fixed `PresentTicket`. The real driver call then executes unchanged. Only a successful/suboptimal present completes the ticket and publishes CPU or precision feedback. This ordering prevents a separate acquire thread from recycling the same image and replacing its mapping before the old present post-hook observes it. Failed presents fail open: the claimed stale mapping is discarded and no completion sample is fabricated.

Swapchain-lane identity is now release-published only after lane payload and precision-profile fields have been initialized. Present-precision dispatch pointers, swapchain flags and device identity are cached once per lane behind a small publication state, so the pending-acquire path no longer rescans the global registry on every frame. Cache contents are immutable until swapchain teardown.

The eight-slot precision ring now prefers an actually free slot instead of selecting one through a global sequence cursor and unconditionally replacing it. A reserved publication sentinel protects payload rewrite. Only when every real slot is occupied may an older sample be displaced to its existing CPU fallback. This removes the cursor RMW and reduces avoidable precision degradation under uneven retirement.

Acquire-side wait probing has an explicit rotating budget of four slots per call. The zero-timeout wait APIs remain sensors, not blockers, but a single acquire can no longer burst through all eight pending entries. `VK_EXT_present_timing` is queried only if at least one live slot actually requested timing metadata; a pending wait-only sample therefore does not pay an unrelated timing driver call.

These changes remain CPU-side orchestration only. Opt5 does not inject `vkQueueSubmit*`, call queue/device idle for pacing, allocate command buffers, enable extensions, modify swapchain flags, or create a second scheduler.
