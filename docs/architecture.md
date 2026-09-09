# Architecture

## Design objectives

Vulkanized-Fakenvapi should keep four responsibilities separate:

1. **Input / interception** — receive the game's NVAPI / Reflex-facing calls.
2. **Capability detection** — determine what the runtime can actually support.
3. **Policy** — decide which backend should be used.
4. **Execution** — invoke the selected backend without leaking policy into every hot path.

## Conceptual flow

```text
Game
 │
 ▼
NVAPI / Reflex-facing layer
 │
 ▼
Capability Detection ───── Configuration
 │                         │
 └────────────┬────────────┘
              ▼
        Backend Policy
              │
      ┌───────┼────────┬──────────┬───────────┐
      ▼       ▼        ▼          ▼           ▼
   Reflex  Anti-Lag  Vulkan AL+  XeLL    LatencyFlex
      │       │        │          │           │
      └───────┴────────┴──────────┴───────────┘
                       │
                       ▼
                     Vulkan
```

## Runtime state model

Avoid conflating these states:

- `compiled`
- `detected`
- `available`
- `selected`
- `active`
- `failed`
- `fallback`

A backend may be compiled and detected but still fail activation.

## Policy rules

The policy should be deterministic.

Recommended order of operations:

1. parse configuration,
2. enumerate backend capabilities,
3. validate explicit user override,
4. apply policy / priority,
5. activate selected backend,
6. fall back only if configured,
7. expose final state to diagnostics / overlay.

## Hot-path constraints

- no unnecessary allocation,
- no repeated capability probing,
- no high-volume logging by default,
- avoid locks where frame-local state can be represented more cheaply,
- avoid vendor-specific assumptions in generic policy code.

## Failure behavior

A failed backend activation must not be reported as active.

If fallback is disabled, fail visibly and retain a diagnostic reason.

If fallback is enabled, record both the requested backend and the selected fallback.
