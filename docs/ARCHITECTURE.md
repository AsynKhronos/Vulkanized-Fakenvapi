# Vulkanized-Fakenvapi Architecture

## Core rule

```text
OBSERVE MANY DURING STARTUP
    -> SELECT ONE SOURCE PER SEMANTIC ASPECT
    -> FREEZE THE OWNERSHIP MAP
    -> DRIVE EXACTLY ONE EXECUTION BACKEND
```

Input instrumentation and execution are different roles. Reflex, XeLL, Anti-Lag 2 and `VK_NV_low_latency2` may coexist as information sources. During startup the Hybrid layer scores each semantic aspect once and freezes provenance for the backend session; gameplay never adaptively switches owners.

## 0.9 Input/output capability recognizer

```text
INPUT FRONTENDS                         OUTPUT BACKENDS
Reflex / XeLL / AL2 / VKLL2            D3D11 / D3D12 / Vulkan
        |                                      |
        v                                      v
capability/context/control/sleep        concrete device probes
marker/async/FG evidence                module/interface/extension/feature
        |                                      |
        v                                      v
InputRecognition                        OutputRecognizer
quality + freshness + evidence          available / unavailable / unknown
        |                                      |
        +---------------+----------------------+
                        v
               startup routing/freeze
                        v
               one execution backend
```

Input capability exposure is intentionally weak evidence. A support query or synthetic context can identify a possible frontend, but cannot make it fresh or eligible by itself; actual frame activity is still required. Control/Sleep/marker/FG evidence accelerates confidence without creating a second pacing owner.

Output recognition is tri-state. `Available` means the process/device exposed positive evidence, `Unavailable` is reserved for authoritative negative proof, and `Unknown` is deliberately retained in the fallback list. Explicit output requests remain authoritative; with fallback disabled, a proven-unavailable forced backend fails closed rather than silently changing technology. This prevents an incomplete startup probe from removing a backend whose own initializer could still succeed. D3D12 probes the concrete device for vkd3d interop and enabled `VK_AMD_anti_lag`, the Anti-Lag 2 device interface, XeLL module/library presence and the software fallback. Native Vulkan reads the already-populated `VulkanDeviceRegistry`. All such work stays on the initialization slow path.


## 0.9 VulkanFlex — Vulkan-exclusive hybrid execution

Native Vulkan cannot use XeLL as the universal execution backend. VulkanFlex fills that execution-role gap without introducing a vendor-only driver dependency. It embeds the LatencyFleX scheduler and consumes two classes of input while preserving exactly one pacing owner.

```text
                  GAME-FACING INPUTS
      Reflex | VKLL2 | XeLL | Anti-Lag2
                    |
                    v
           startup HybridFusion
      pacing/render/present/input/control
          each source frozen once
                    |
                    +------------------+
                                       |
CORE VULKAN WSI                        v
vkAcquireNextImage* ------------> VulkanFlex executor
vkQueuePresentKHR  ------------->   |
                                    v
                            LatencyFleX scheduler
```

When no game-facing low-latency frontend becomes the locked pacing source, a successful image acquire becomes VulkanFlex's late frame-begin/pacing opportunity and the corresponding queue-present return becomes CPU-side frame feedback. The swapchain-to-frame relation uses fixed per-swapchain image-index atomics; present feedback uses fixed atomic slots. There is no per-frame heap allocation, no blocking scheduler mutex and no second Vulkan device/queue owner. Scheduler contention is fail-open.

If an explicit frontend supplies per-frame Sleep calls, those calls become the pacing owner. `SIMULATION_START` remains semantic input but is prevented from issuing a second sleep. Hybrid marker acceptance is evaluated per semantic aspect on Vulkan just as on D3D12.

Auto policy does not run VulkanFlex behind native `VK_NV_low_latency2` when native Vulkan low-latency extensions are preferred. Explicit `output.vulkan=vulkanflex` is the opt-in override. `only_native_vulkan=1` also avoids automatically taking over obvious D3D-to-Vulkan translation/proxy stacks.

The core-WSI timing tier must not be interpreted as physical presentation completion. `vkQueuePresentKHR` only provides a portable submission/return boundary. Standard present-wait/present-timing extensions can be added later as higher-fidelity feedback without changing the single-owner architecture.

## 0.9 low-level Vulkan/VKD3D capability layer

The D3D12 recognizer and direct execution backend now share one concrete vkd3d-proton/Vulkan capability probe. It starts from the real `ID3D12Device`, proves `ID3D12DXVKInteropDevice`, optionally recognizes the newer Device2 interface, borrows the existing `VkInstance` / `VkPhysicalDevice` / `VkDevice`, then snapshots device identity, queue-family topology and enabled low-level extensions/features. No Vulkan device or queue is created by the probe.

```text
ID3D12Device
    | QueryInterface
    v
ID3D12DXVKInteropDevice [ + optional Device2 ]
    |
    +--> enabled extension/feature chain
    +--> VkInstance / VkPhysicalDevice / VkDevice
                              |
                              +--> VkPhysicalDeviceProperties
                              +--> VkQueueFamilyProperties
                              +--> direct device dispatch (vkAntiLagUpdateAMD)
```

This lets policy reason from actual translation-layer/device capabilities instead of process-level DLL heuristics. The queue-notify hot path also resolves queue -> device -> native dispatch through thread-local caches after warm-up; the mutex-backed global scan remains only as a compatibility fallback for unobserved queues.

## 0.8 D3D12 low-level execution

```text
Game frontends (Reflex / AL2 / XeLL)
             |
             v
   startup-frozen Hybrid state
             |
             v
      D3D12 backend policy
        /             \
prefer native        fallback
      |                 |
      v                 v
ID3D12DXVKInterop   XeLL / AL2 / LFX
      |
      v
 existing VkDevice
      |
      v
vkAntiLagUpdateAMD
```

The direct AMD path is deliberately narrow. It is selected only for D3D12 auto-routing when `[vulkan] prefer_native_extensions=1`, and initialization must prove all of the following: the D3D12 device exposes vkd3d-proton's interop interface, the existing Vulkan device has `VK_AMD_anti_lag` enabled, the returned feature chain reports `antiLag = VK_TRUE`, and `vkAntiLagUpdateAMD` resolves from that exact device. Failure at any gate falls through to the configured high-level backends.

No `vkCreateDevice`, Vulkan-core detour or second loader ownership is introduced by this backend. It borrows vkd3d-proton's already-created Vulkan device and resolves the device command once during backend initialization. Frame execution uses fixed-size/atomic state only.

The input stage initially uses the base `VkAntiLagDataAMD` form without presentation metadata. Only after exact canonical input/present frame IDs have been observed to match does the backend emit paired `VK_ANTI_LAG_STAGE_INPUT_AMD` / `VK_ANTI_LAG_STAGE_PRESENT_AMD` updates. This avoids inventing a frame-index relationship.

Direct D3D12 `VK_NV_low_latency2` execution is intentionally out of scope because the Vulkan commands are tied to a real `VkSwapchainKHR`; vkd3d device interop alone is insufficient.

### 0.8 steady-state hot path

- unchanged D3D backend/API/device/routing state returns through atomic loads before `active_tech_mutex`;
- once Hybrid startup ownership is locked, the runtime no longer executes the input selection CAS/scoring loop per marker;
- FG and forced-enable backend setters execute only when their effective state changes;
- repeated empty `GetLatency()` polls log once, not once per call;
- release (`NDEBUG`) builds compile frame-hot trace formatting/calls out through `VFN_HOT_TRACE`;
- direct `VK_AMD_anti_lag` control state is atomic and interval-to-FPS conversion is integer-only.


## 0.4 Vulkan input pipeline

```text
Vulkan extension enumeration
        |
        +-- native VK_NV_low_latency2 ----> observed native pass-through
        |
        +-- emulated VK_NV_low_latency2 --> normalized MarkerType
                                              |
                                              v
                                         InputArbiter
                                              |
                                              v
                                      selected_input only
                                              |
                                      +-------+-------+
                                      |               |
                               VK_AMD_anti_lag   LatencyFlex
```

`vkEnumerateDeviceExtensionProperties` is intercepted only for the driver-level list. If policy exposes `VK_NV_low_latency2` on a driver that does not implement it, the extension is advertised to the application but removed from the copied driver-facing `VkDeviceCreateInfo` before the real `vkCreateDevice` call. Caller-owned structures are never modified.

`vkGetDeviceProcAddr` and `vkGetInstanceProcAddr` return Vulkanized-Fakenvapi wrappers for the low-latency2 commands. The wrappers translate sleep/mode/marker calls into the same `InputArbiter` used by Reflex, XeLL and Anti-Lag 2. Timeline-semaphore signalling is preserved even when another frontend wins arbitration, preventing an unselected low-latency2 sleep path from stalling the application.

Cross-vendor exposure is dependency-gated. Emulation is advertised only when the physical device can satisfy the extension dependency set: Vulkan 1.2 or `VK_KHR_timeline_semaphore`, and `VK_KHR_present_id` or `VK_KHR_present_id2`. Device creation additionally verifies the required device extensions are enabled. Low Latency 2 device groups are rejected rather than silently emulated with incorrect semantics.

`vkLatencySleepNV` remains asynchronous as required by the Vulkan API contract. The caller thread records the input observation and queues a fixed-size sleep job; one worker drives the selected Vulkan pacing backend and then signals the application-provided timeline semaphore. Queue saturation fails open by signalling immediately, avoiding an application deadlock. The marker hot path itself performs no allocation, filesystem access or timer polling.

When the driver natively supports `VK_NV_low_latency2` and policy prefers native extensions, native function pointers are retained per device. Marker/sleep calls are observed by the arbiter and forwarded only when that frontend is selected, so native Reflex does not run beside another selected pacing path.

### Delayed Vulkan loader discovery

If `vulkan-1.dll` is absent during `DLL_PROCESS_ATTACH`, lightweight Detours hooks are installed on the LoadLibrary family. After any later library load, the watcher probes for `vulkan-1.dll` and installs the Vulkan hooks before control returns to the caller. No polling thread or timer is used.

## 0.3 D3D input pipeline

```text
NVAPI Reflex -----------+
                        |
XeLL game API ----------+--> normalized MarkerType
  synthetic context     |        |
                        |        v
Anti-Lag 2 DX11/DX12 ---+--> InputArbiter
  game-facing proxies            |
                                 v
                         selected_input
                                 |
                                 v
                     Shared LowLatency context
                                 |
                                 v
                     ONE output backend only
```

### XeLL input/output separation

Game-facing XeLL context creation returns a synthetic opaque context. Calls on
that context are translated to the shared frontend bridge. The real XeLL DLL
remains loaded and hooked, but calls originating from Vulkanized-Fakenvapi's
XeLL output backend are forwarded through the Detours trampoline to the real
SDK. This prevents the same game marker from becoming two pacing paths.

### Anti-Lag 2 input/output separation

For D3D12, requests for `IID_IAmdExtAntiLagApi` receive a COM-compatible proxy.
For D3D11, the Anti-Lag request passed to `AmdDxExtCreate11` receives the
corresponding DX11 proxy. The proxies observe/update the common frontend state
without invoking native game AL2 pacing.

When the selected output backend itself is Anti-Lag 2, initialization enters an
explicit pass-through section. The hook then returns the real AMD interface to
the backend, so translated output remains possible without recursive proxying.

### Shared output ownership

0.2 had enough infrastructure for separate callers but XeLL still had a
separate `LowLatency` context. 0.3 removes that ownership split. Reflex, XeLL
and Anti-Lag 2 now share the same active backend pointer and routing state.
Therefore backend initialization/deinitialization is globally serialized for
these D3D frontends.

## Arbitration freshness and hysteresis

Each observed marker increments one global observation sequence. Every frontend
stores the sequence number of its latest marker. A source is stale when another
source has advanced more than `kStaleObservationWindow` observations since that
marker.

This gives stale-source decay without reading a clock or issuing a syscall in
the marker hot path.

Switching also uses `kSwitchQualityMargin`: a healthy current frontend is kept
unless a challenger is materially better. Equal-quality ties may move toward
the configured `[input] priority`, so user policy remains deterministic at
steady state.

## Runtime policy

Configuration parsing remains control-plane work. Published
`RuntimePolicySnapshot` objects are immutable and process-lifetime stable; hot
readers use one atomic pointer load. Fixed-size arrays are used for input and
backend order.

## Output defaults

```text
D3D11:  Anti-Lag 2 -> LatencyFlex
D3D12:  Anti-Lag 2 -> XeLL -> LatencyFlex
Vulkan: VK_AMD_anti_lag -> LatencyFlex
```

## Vulkan frontend status

`0.4.0-vulkan-input` implements native/emulated `VK_NV_low_latency2` interception, common-input arbitration and delayed Vulkan-loader discovery. Remaining Vulkan work is runtime-hardening around optional low-latency2 pNext structures/capability queries and broader game validation; it is no longer a missing frontend.

## Game-facing capability emulation (0.5.0)

Capability exposure is a separate plane from runtime backend routing:

```text
GAME UI / SUPPORT PROBES
  Reflex (Streamline)  XeLL  Anti-Lag 2
           |            |       |
           +---- capability ----+
                    |
              INPUT FRONTENDS
                    |
             Common LL Model
                    |
                Arbiter
                    |
            ONE OUTPUT BACKEND
        AL2 / XeLL / LatencyFlex
```

A game may therefore select **Reflex** or **XeLL** in its own menu while the
selected output backend is a different cross-vendor implementation. Capability
spoofing must never imply multiple active pacing backends.


## Startup-Locked Per-Aspect Hybrid XeLL (0.7.0-r1)

```text
Reflex --------\
Game XeLL ------+--> STARTUP evidence / per-aspect scoring --> FREEZE OWNERS --\
Anti-Lag 2 -----+                                                           +--> Canonical LL State --> XeLL --> ONE Sleep
VK LL2 --------/------------------------------------------------------------/
```

There is no monolithic primary/secondary input. The runtime collects startup evidence from all available frontends, including game-originated XeLL, but does not permit provisional XeLL pacing while the ownership map is unresolved.

After the startup observation window, one source is frozen for each semantic aspect: pacing, enabled state, Boost, minimum interval, marker optimization, simulation/render/present lifecycle, input sampling, out-of-band lifecycle and FG metadata. These owners never switch during gameplay. A backend teardown/reset starts a new discovery epoch.

`Pacing` remains the only aspect allowed to invoke backend `Sleep()`. In Hybrid XeLL mode the locked pacing source creates a monotonic internal Canonical FrameToken. XeLL Sleep and every accepted semantic marker are translated onto that token. Cross-source Reflex / Anti-Lag 2 / XeLL numeric IDs are therefore independent source metadata and never require numeric equality.

## VulkanFlex R3.3 transport split

VulkanFlex now has two execution transports with different ownership rules. Native Vulkan uses the WSI Acquire/Present bridge. D3D12 on vkd3d-proton never installs Vulkan detours; it discovers the public `ID3D12DXVKInteropDevice`, borrows the existing Vulkan handles, and resolves D3D12 queues with `GetVulkanQueueInfo`. HybridFusion stays above both transports and remains the semantic source arbiter.

```text
Native Vulkan WSI ----> VulkanFlex executor
                           ^
                           |
HybridFusion -------------+
                           |
D3D12 markers -> VKD3D COM interop -> borrowed VkDevice/VkQueue identity
```

The bridge is observational/control-plane only with respect to Vulkan. It never submits work or takes ownership of VKD3D queue synchronization.

## VulkanFlex R3.4 executor/observer split and FrameToken timeline

D3D12 and native Vulkan no longer use the same VulkanFlex execution policy. Native Vulkan keeps VulkanFlex as an executor. D3D12 prefers mature D3D12 pacing (direct `VK_AMD_anti_lag`, then XeLL, then Anti-Lag 2); VulkanFlex becomes a capability-gated fallback executor. If VKD3D interop exists while another D3D12 executor is active, a fixed VulkanFlex observer instance attaches only to the transport/telemetry plane.

```text
Reflex / AL2 / XeLL native frame IDs
                 |
                 v
       Canonical FrameToken
       epoch + sequence + transport
                 |
          HybridFusion aspects
                 |
        +--------+---------+
        |                  |
   XeLL executor      VulkanFlex observer
   one CPU wait       VkDevice/VkQueue/present
```

`HybridFusion` stores 64 fixed token slots plus fixed per-frontend source bindings. The pacing source is the only source allowed to create a token. The common in-order secondary callback binds directly to the current token. If the pacing source is multiple tokens ahead, Opt4 uses monotonic progress inside that secondary frontend's own native-ID domain to recover a bounded skipped token; ambiguous delayed IDs are rejected rather than blindly attached to the newest render frame. A source absent for an entire ring window is explicitly resynchronized because exact historical correlation no longer exists. Existing delayed IDs still resolve through their O(1) binding. This removes the former implicit raw-ID equality contract without adding a map, vector, mutex or heap allocation to the marker hot path.

The frozen per-aspect ownership matrix is packed into one 64-bit read-mostly routing word after startup. `InputSample`, `TriggerFlash`, and `PcLatencyPing` have independent owners: AL2 may therefore supply the best input-sampling signal while Reflex or VK-LL2 supplies flash/ping telemetry on the same canonical frame. Start/End lifecycle groups remain paired deliberately and require complete startup evidence before ownership can freeze.

XeLL lifecycle state is now context-local: exact slept frame IDs replace sticky modulo booleans, the D3D12 QueryInterface reference is released after context creation, unloading clears the module handle, and sleep-mode deduplication is reset for every new context.


## VulkanFlex R3.8 render/presentation timeline split

R3.8 formalizes two independent canonical domains:

```text
RenderToken R42
   ├── Presentation P80 generation=1 interpolated=false
   └── Presentation P81 generation=2 interpolated=true

RenderToken R43
   └── Presentation P82 generation=1 interpolated=false
```

The render token remains the sole pacing identity. Presentation tokens are semantic/telemetry identities only. HybridFusion owns the mapping with fixed rings and epoch tags; no generated presentation may trigger `sleep`, `LatencySleep`, `pace_frame`, Vulkan queue submission, or an independent present wait. This preserves the single-owner architecture while making FG first-class.

The OptiScaler `Fake_InformPresentFG` callback now enters the Reflex frontend domain and participates in startup-locked FrameGeneration source arbitration. VKD3D and DXVK observers may consume the canonical mapping without owning presentation or scheduling.

## VulkanFlex R3.7 queue-pressure governor

```text
R3.6 precise completion evidence
           |
           v
 fixed pending-present slots ----> precise/fallback trust
           |                               |
           +---------- backlog ------------+
                           |
                     Queue Pressure
                           |
              adaptive bounded delay
                           |
                           v
       max(LatencyFleX target, governor target)
                           |
                           v
                  ONE VulkanFlex sleep
```

The governor is part of the native-Vulkan executor, not a second backend. Auto mode requires sustained trustworthy VF2+ completion evidence before it can affect the wake target. Translation transports are excluded: VKD3D remains an observer behind the selected D3D12 executor and DXVK remains delegate/observer according to R3.5 ownership. No Vulkan submit interception is required; presentation-engine backlog is the authoritative pressure signal available from the existing precision layer.
## VulkanFlex R3.9 capability plane

Capability discovery is explicitly orthogonal to semantic input and pacing ownership:

```text
Native VkDeviceCreateInfo ---- exact enabled state ---\
VKD3D public interop -------- exact exposed state -----+--> VulkanCapabilitySnapshot
DXVK public interop --------- support / unknown -------/            │
                                                                  telemetry/policy
                                                                       │
                                                                       ▼
                                                             existing single owner
```

The snapshot records Vulkan 1.3/1.4 facilities and granular EDS/pipeline/present features without mutating any of them. `supported`, `enabled` and `enabled_known` are separate so a translation transport cannot accidentally upgrade "not observable" to "enabled". EDS3 is represented by per-feature masks; EDS1 and the core Vulkan-1.3 EDS2 command set remain distinct from EDS2 logic-op and patch-control-points extension features. The device registry publishes the fixed snapshot alongside existing Vulkan dispatch state. No R3.9 path records commands, submits work, edits device-create data or changes scheduler ownership.

## VulkanFlex R4.0 adaptive scheduler

R4.0 adds a rare-path structural evidence plane beside the existing presentation/queue telemetry. Native Vulkan pipeline-creation calls are timed but never rewritten. Long-call evidence is correlated with completion gaps only after the clean-frame baseline is warm. This produces a bounded compensation interval, not an alternative frame-time estimate.

The LatencyFleX clock remains the system clock: `CompensateExternalStall()` advances only historical references that overlap the confirmed pause interval. Consequently the compile pause disappears from estimator deltas while future wake targets remain in the real timestamp domain. Pipeline evidence is epoch-tagged and fixed-storage. Translation observers do not enable this execution-time shielding.

## VulkanFlex R4.0.1 authoritative transport truth

Transport identity and Vulkan-hook ownership are now separate control-plane facts. `TransportTruth` publishes three kinds of state through lock-free atomics: a directly proven application transport, the Vulkan hook-chain disposition, and accumulated evidence bits. The early Wine/module classifier may publish `TranslationBypassSuspected`; it never publishes VKD3D/DXVK as authoritative application transport. Public VKD3D/DXVK COM interop or an intercepted native `VkDevice` provides the strong evidence.

`NativeExecutionAccess` has three outcomes. `FullWsi` requires actual native Vulkan hook-chain ownership. `ExplicitOnly` preserves explicit NVAPI/Vulkan marker/sleep pacing when WSI ownership is unavailable or only heuristically bypassed. `Blocked` prevents a separate native-Vulkan executor after VKD3D/DXVK transport is directly proven. Strong evidence for multiple transports becomes `Mixed`, allowing local active-API arbitration rather than globally discarding one real route.

An active confirmed translation route also shadows Vulkan frontend callbacks. The callback returns without changing backend ownership, which prevents a Vulkan-shaped proxy callback from tearing down XeLL/Anti-Lag 2 on D3D12 or DXVK's delegated D3D11 route. The transport-truth plane performs no allocation, Vulkan command, queue submission, COM call or frame pacing itself.

## R4.2 canonical host clock

The shared graphics/XR host clock is QueryPerformanceCounter scaled to nanoseconds. XRFlex converts each relevant runtime `XrTime` through `xrConvertTimeToWin32PerformanceCounterKHR` on the owning `XrInstance`, then compares the resulting QPC-domain timestamp with graphics/XR hook timestamps from `get_timestamp()`. No persistent epoch offset is inferred because OpenXR does not guarantee a constant relationship between runtime time and system clocks across frames.

## R4.3 AudioFlex domain

AudioFlex is a separate observer domain. Discovery begins at `CoCreateInstance(CLSID_MMDeviceEnumerator)` and follows MMDevice enumerator/collection results to `IMMDevice::Activate`, then dynamically attaches to the concrete WASAPI COM method implementations. The observer records successful `IAudioClient` / `IAudioClient3` configuration calls and high-frequency padding/clock samples into fixed atomic state. It never substitutes arguments or calls an audio initialization method on behalf of the application.

## R4.4 Audio queue model and getter-only probe

R4.4 treats shared-mode render padding as **endpoint queued-audio evidence**, not complete acoustic latency. The model converts padding and buffer capacity using the client stream rate, keeps the instantaneous shared-engine sample rate separate for current-period conversion, and carries stream-latency/engine-period/clock evidence as independent fields. Capture and exclusive streams are intentionally outside the R4.4 playback-queue claim.

When enabled, a one-shot post-initialize probe invokes getter-only COM methods on the already initialized client. It may obtain `IAudioClient3` and `IAudioClock`, but it does not initialize a new stream, modify client properties, request a smaller period or touch `IAudioRenderClient`. If the application continues calling `GetCurrentPadding`, AudioFlex refreshes `IAudioClock::GetPosition` no more frequently than the configured probe interval (250 ms default). The periodic probe uses fixed per-client atomic deadlines and does not log per sample.

The audio clock remains correlated to canonical QPC nanoseconds. Queue state is summarized through fixed counters (sample count, total queued ns, maximum queued ns), keeping the padding hot path allocation-free and independent from Vulkan/XeLL/OpenXR timing ownership.


## R4.5 Audio period negotiation

The R4.5 mutation boundary is deliberately narrow. Only an application-originated `IAudioClient3::InitializeSharedAudioStream` call is eligible. Before forwarding it, AudioFlex may query `GetSharedModeEnginePeriod` on the same interface and calculate a supported candidate. Auto mode records/logs that candidate without changing arguments. Explicit mode may replace only `PeriodInFrames`. The candidate is aligned upward to `fundamentalPeriod`, clamped to the advertised minimum/maximum, never exceeds the application's requested period, and is limited by a configurable one-step reduction percentage.

AudioFlex does not retry initialization after a negotiated failure because Core Audio does not guarantee that a failed initialization leaves the interface safely re-initializable. Classic `IAudioClient::Initialize`, stream state, render buffers, format, device selection and graphics/XR owners remain outside this policy.
