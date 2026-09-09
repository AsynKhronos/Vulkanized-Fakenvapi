# Configuration

> The configuration schema below is a design target. Keep this document synchronized with the actual parser.

## Proposed structure

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

## `latency.backend`

Intended values:

- `auto`
- `reflex`
- `antilag`
- `xell`
- `latencyflex`
- `off`

An explicit backend should not silently become another backend unless fallback behavior is enabled and clearly
reported.

## `latency.backend_priority`

Used only when automatic selection is enabled.

Do not hard-code a universal "best" ordering into documentation until benchmark and compatibility data justify it.

## `latency.fallback`

- `true`: allow policy to select another usable backend after activation failure.
- `false`: stop backend activation and expose a clear diagnostic state.

## Overlay

The startup overlay is intended for short capability / state reporting, not for permanent performance telemetry.

## Logging

Suggested levels:

- `off`
- `error`
- `warn`
- `info`
- `debug`
- `trace`

`trace` should never be the default because it can distort timing-sensitive behavior.
