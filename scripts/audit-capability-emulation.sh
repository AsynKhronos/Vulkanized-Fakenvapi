#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: game-facing Reflex/XeLL capability emulation audit"; }

[[ -f src/streamline_capability.cpp ]] || fail "missing Streamline capability module"
grep -q 'kFeatureReflex = 3' src/streamline_capability.cpp || fail "Reflex feature id not pinned"
grep -q 'slIsFeatureSupported(Reflex) -> supported' src/streamline_capability.cpp || fail "Reflex support override missing"
grep -q 'lowLatencyAvailable = true' src/streamline_capability.cpp || fail "Reflex state availability override missing"
grep -q 'slReflexGetState' src/streamline_capability.cpp || fail "Reflex state function wrapping missing"
grep -q 'slGetFeatureFunction' src/streamline_capability.cpp || fail "Streamline feature-function hook missing"
grep -q 'struct ReflexStatePrefix' src/streamline_capability.cpp || fail "Reflex state ABI prefix missing"

grep -q 'called_by_game_family' src/fakexell.cpp || fail "XeLL game-family caller classification missing"
grep -q 'same_directory(caller_path, exe_path)' src/fakexell.cpp || fail "XeLL game DLL support missing"
grep -q 'infrastructure_module' src/fakexell.cpp || fail "XeLL infrastructure exclusion missing"
grep -q 'XeLL game-facing capability: synthetic D3D12 context created' src/fakexell.cpp || fail "XeLL capability diagnostic missing"
grep -q 'hkxellGetFramesReports' src/fakexell.cpp || fail "synthetic XeLL frame-report handling missing"

# Capability emulation must not globally impersonate Intel/NVIDIA through DXGI;
# output backend choice remains independent from game-facing capability.
! grep -q 'spoof_intel = true' src/streamline_capability.cpp || fail "global Intel spoof introduced"

pass
