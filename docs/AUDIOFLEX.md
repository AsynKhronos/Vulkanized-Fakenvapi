# AudioFlex R4.5 — queue model + opt-in period negotiation

AudioFlex remains a separate domain beside VulkanFlex and XRFlex. R4.5 preserves the R4.4 getter-only queue/clock model and adds a narrowly scoped initialization-time negotiation path only when the application itself calls `IAudioClient3::InitializeSharedAudioStream`. Auto mode remains non-mutating; explicit enablement is required before `PeriodInFrames` can be lowered.

## Runtime flow

```text
IMMDevice::Activate
        |
   IAudioClient
        |
  Initialize succeeds
        |
        +-- record application-selected format/buffering
        +-- getter-only active probe (optional)
        |      +-- GetBufferSize
        |      +-- GetStreamLatency
        |      +-- GetDevicePeriod
        |      +-- GetCurrentPadding
        |      +-- QI IAudioClient3
        |      |      +-- GetSharedModeEnginePeriod
        |      |      +-- GetCurrentSharedModeEnginePeriod
        |      +-- GetService(IAudioClock)
        |             +-- GetFrequency
        |             +-- GetPosition
        |
  application padding calls
        |
        +-- fixed-state padding update
        +-- throttled clock refresh (default <= 4 Hz)
        +-- endpoint queue model
```

All active-probe calls are read-only/query operations. `GetCurrentSharedModeEnginePeriod` returns a COM-allocated format block; R4.4 frees it with `CoTaskMemFree` after reading the instantaneous engine format/period.

## Queue semantics

R4.4 labels queue latency only for **shared-mode render streams**. In that case WASAPI padding is the count of valid endpoint-buffer frames queued to play. The model reports:

- endpoint queued frames/time,
- endpoint buffer capacity and available capacity,
- fill ratio in permille,
- current/default engine period,
- `GetStreamLatency` as separate maximum stream-latency evidence,
- padding sample age,
- `IAudioClock` position/frequency and QPC correlation,
- confidence tier: padding / buffered / clock-correlated.

The model deliberately does **not** add these fields together into a single "speaker latency" number. Hardware/DAC transport, mixer behavior and downstream device buffering are outside the evidence available here.

The client stream sample rate and current shared-engine sample rate are stored separately. This matters when the engine/mix format differs from the application's stream format: padding-to-time conversion keeps using the client stream rate, while current engine-period frames use the engine rate returned by `GetCurrentSharedModeEnginePeriod`.

## Active clock refresh

A game may call `GetCurrentPadding` thousands of times while never requesting `IAudioClock`. R4.4 therefore permits a bounded getter-only clock refresh when `active_probe=1` and `clock=1`. The padding hook uses a fixed per-client atomic next-probe deadline and asks for `IAudioClock::GetPosition` only after that deadline. Default interval is 250 ms.

No COM clock reference is retained across those periodic probes; `GetService` is called, the position is sampled and the returned service reference is released. This avoids a persistent interface-lifetime dependency in the audio hot path.

## Configuration

```ini
[audio]
enabled=1
wasapi_observer=1
clock=1
active_probe=1
queue_model=1
clock_probe_interval_ms=250
adaptive_period=auto
period_target_us=5000
period_max_reduction_percent=50
```

`adaptive_period=auto` computes/logs a supported recommendation only. `adaptive_period=1` permits a one-shot lower-period substitution on an application-originated `InitializeSharedAudioStream`; `0` disables negotiation. The target is clamped to 500..50000 us and max reduction to 10..90%. `active_probe=0` restores pure application-driven observation. `queue_model=0` retains raw telemetry without queue estimates. `clock=0` disables both application-driven and active AudioClock sampling. The probe interval is clamped to 50..5000 ms.

## Runtime diagnostics

Expected R4.4 lines include:

```text
AudioFlex WASAPI observer armed: ... active_probe=true, queue_model=true, clock_probe_interval_ms=250, mutation=false, adaptive_period=false
AudioFlex WASAPI stream initialized: ... mode=shared ... observer_only=true
AudioFlex active probe: ... period_valid=..., clock_valid=..., buffer_frames=..., current_period_frames=..., mutation=false
AudioFlex queue model active: ... endpoint_queued_us=..., capacity_us=..., fill_permille=..., engine_period_us=..., stream_latency_us=..., clock_correlated=..., endpoint_queue_only=true
AudioFlex shutdown: ... queue_samples=..., queue_avg_us=..., queue_max_us=..., dropped=...
```

## Ownership contract

R4.5 never creates a new stream on behalf of the application, never converts classic `IAudioClient::Initialize` into an `IAudioClient3` call, never calls `SetClientProperties`, never obtains `IAudioRenderClient`, never writes/releases render buffers, never resamples and never sleeps. In explicit mode it may alter only the `PeriodInFrames` argument of the application's own `InitializeSharedAudioStream` call.

A negotiated initialization is attempted exactly once. There is no fallback retry on the same client after failure because WASAPI documents that a failed initialization can still leave later initialization attempts returning `AUDCLNT_E_ALREADY_INITIALIZED`. Auto mode therefore remains recommendation-only.

`ActivateAudioInterfaceAsync` remains outside this release and requires a separate completion-handler-safe interception design.
