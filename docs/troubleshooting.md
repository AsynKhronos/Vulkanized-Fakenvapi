# Troubleshooting

## The project does not appear to load

Check:

1. loader / DLL placement,
2. Wine / Proton prefix,
3. the game's graphics API,
4. whether another NVAPI replacement is taking precedence,
5. logs with an appropriate debug level.

## The configured backend is not active

Do not assume configuration equals activation.

Check:

- backend capability detection,
- required Vulkan extensions,
- GPU / driver support,
- backend initialization result,
- whether fallback selected another backend,
- frame-generation constraints.

## Performance became worse

Collect comparable data before filing an issue:

- identical game scene,
- identical graphics settings,
- identical frame cap,
- same Wine / Proton version,
- same driver,
- same backend,
- multiple runs.

Include frame-time data where possible. Average FPS alone can hide latency and pacing regressions.

## Online / anti-cheat game

Do not assume injected or substituted libraries are safe to use with anti-cheat systems. Follow the game's rules
and avoid risking an account on unsupported configurations.
