# XRFlex R4.2

XRFlex is the OpenXR timing/telemetry domain of Vulkanized-Fakenvapi. R4.2 remains deliberately observer-only: the OpenXR runtime owns frame-loop synchronization and throttling, while XRFlex adds a standards-based canonical clock bridge.

## Interception surface

R4.2 observes:

- `xrGetInstanceProcAddr`
- `xrCreateSession`
- `xrWaitFrame`
- `xrBeginFrame`
- `xrEndFrame`
- `xrDestroySession`

Both direct loader exports and function pointers obtained through `xrGetInstanceProcAddr` are covered. XRFlex calls the original runtime/loader function first and records only successful results.

## Frame token

Each successful `xrWaitFrame` creates an `XrFrameToken` with:

```text
epoch
sequence
session
predicted_display_time
predicted_display_period
submitted_display_time
wait_return_ns
begin_return_ns
end_return_ns
render_epoch
render_sequence
should_render
begun
ended
discarded
```

The original OpenXR time values remain stored verbatim in the runtime `XrTime` domain. When the Win32 time-conversion extension is available, R4.2 additionally stores independently converted QPC-domain host timestamps so cross-clock arithmetic never assumes equal epochs.

## Render correlation

At successful `xrWaitFrame`, XRFlex snapshots the latest canonical render sequence as a watermark. At successful `xrEndFrame`, it may associate the XR token with the latest canonical render token only when that render sequence is newer than the watermark.

This prevents a newly waited XR frame from inheriting the previous render frame. The mapping is intentionally best-effort; R4.2 does not assume XR frame sequence, Reflex IDs, Vulkan present IDs or canonical render IDs are equal.

## Ownership contract

```text
OpenXR runtime: xrWaitFrame synchronization/throttling owner
XRFlex:         observer only
VulkanFlex:     existing graphics executor/observer rules
XeLL/DXVK:      existing single-owner rules
```

R4.2 never adds a second XR wait, sleeps the application, changes `predictedDisplayTime`, changes `predictedDisplayPeriod`, rewrites `displayTime`, submits Vulkan work or waits on a Vulkan presentation primitive.

## Fixed storage

The timeline uses 16 fixed session slots and 16 fixed frame slots per session. Session keys and frame sequences are atomic publication words. Session destruction increments the epoch before releasing the slot. A full/colliding pathological state drops telemetry rather than blocking.

## Configuration

```ini
[openxr]
enabled=1
timeline=1
clock_sync=1
```

`enabled` controls OpenXR hook discovery/installation. `timeline` controls frame observation. `clock_sync` enables per-timepoint standard XrTime→QPC conversion when the concrete runtime instance exposes it. The normal release default enables all three.

## Expected runtime diagnostics

```text
XRFlex OpenXR hooks installed: ... clock_sync=true, observer_only=true
XRFlex session clock capability: ... XR_KHR_win32_convert_performance_counter_time=true, host_clock=qpc
XRFlex OpenXR timeline attached: timing_owner=openxr-runtime, observer_only=true, ...
XRFlex canonical clock active: ... host_clock=qpc, xr_frame=..., wait_to_display_us=...
XRFlex graphics correlation active: xr_frame=..., render_frame=..., end_to_display_us=..., clock_valid=...
XRFlex stats: waits=..., begins=..., ends=..., discarded=..., correlated=..., clocked_waits=..., clocked_ends=..., dropped=...
```

## R4.2 canonical clock

R4.2 defines the project host timing domain as monotonic QueryPerformanceCounter converted to nanoseconds. All existing `get_timestamp()` graphics timing therefore shares the same clock basis used by `XR_KHR_win32_convert_performance_counter_time`.

For each successful `xrWaitFrame`, XRFlex converts `predictedDisplayTime` with `xrConvertTimeToWin32PerformanceCounterKHR` when that command is available for the concrete `XrInstance`. At successful `xrEndFrame` it independently converts `XrFrameEndInfo::displayTime`. The conversion is repeated for each relevant time point; no XrTime-to-host offset is cached.

The conversion extension is not injected or enabled by Vulkanized-Fakenvapi. `xrGetInstanceProcAddr(instance, "xrConvertTimeToWin32PerformanceCounterKHR", ...)` is used after successful `xrCreateSession`, and the clock bridge is simply unavailable when the runtime/instance does not expose the command.

New token telemetry includes `predicted_display_host_ns`, `submitted_display_host_ns`, `wait_to_display_ns`, and `end_to_display_ns`. Positive slack means the observed host event occurred before the corresponding display deadline; negative `end_to_display_ns` means `xrEndFrame` returned after that time point. These values are telemetry only and never drive an extra wait in R4.2.

## Next: R4.3

AudioFlex observer telemetry is next. R4.2 does not add adaptive XR pacing; the OpenXR runtime remains the XR timing owner.
