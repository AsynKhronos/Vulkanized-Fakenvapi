#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

fail=0

# AMD ffx_antilag2_dx12.h is not self-contained: it references ID3D12Device.
# Keep d3d12.h before the vendor header so al2_proxy.cpp compiles as a
# standalone translation unit under GCC/MinGW.
d3d12_line=$(grep -n -m1 '^#include <d3d12.h>$' "$ROOT/src/al2_proxy.h" | cut -d: -f1 || true)
ffx12_line=$(grep -n -m1 'ffx_antilag2_dx12.h' "$ROOT/src/al2_proxy.h" | cut -d: -f1 || true)
if [[ -z "$d3d12_line" || -z "$ffx12_line" || "$d3d12_line" -ge "$ffx12_line" ]]; then
  echo "FAIL: al2_proxy.h must include <d3d12.h> before ffx_antilag2_dx12.h" >&2
  fail=1
fi

if grep -R -n 'LowLatencyCtxXell' "$ROOT/src"; then
  echo "FAIL: XeLL still owns a separate LowLatency output context" >&2
  fail=1
fi

if ! grep -q 'InputFrontend::XeLL' "$ROOT/src/fakexell.cpp" || \
   ! grep -q 'FrontendSetMarker' "$ROOT/src/fakexell.cpp"; then
  echo "FAIL: XeLL frontend is not wired into the common input bridge" >&2
  fail=1
fi

if ! grep -q 'InputFrontend::AntiLag2' "$ROOT/src/al2_proxy.cpp" || \
   ! grep -q 'FrontendSetMarker' "$ROOT/src/al2_proxy.cpp"; then
  echo "FAIL: Anti-Lag 2 frontend is not wired into the common input bridge" >&2
  fail=1
fi

if ! grep -q 'disableAl2Kill.store(true' "$ROOT/src/low_latency_tech/ll_antilag2.cpp"; then
  echo "FAIL: Anti-Lag 2 output backend does not explicitly bypass the game-facing proxy" >&2
  fail=1
fi

if ! grep -q 'kStaleObservationWindow' "$ROOT/src/input_arbiter.h" || \
   ! grep -q 'kSwitchQualityMargin' "$ROOT/src/input_arbiter.h"; then
  echo "FAIL: multi-input freshness/hysteresis guards are missing" >&2
  fail=1
fi

if (( fail != 0 )); then exit 1; fi

echo "PASS: universal input frontend architecture audit"
