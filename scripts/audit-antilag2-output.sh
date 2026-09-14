#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
F="$ROOT/src/low_latency_tech/ll_antilag2.cpp"
P="$ROOT/src/policy_engine.cpp"
R="$ROOT/src/runtime_policy.h"

grep -q 'QueryInterface(' "$F"
grep -q 'IID_IAmdExtAntiLagApi' "$F"
grep -q 'vkd3d-proton/VK_AMD_anti_lag' "$F"
grep -q 'AMD::AntiLag2DX12::Initialize' "$F"
grep -q 'Backend::AmdAntiLagVk' "$P"
grep -q 'Backend::AntiLag2' "$P"
grep -q 'Backend::XeLL' "$P"
grep -q 'value.push_back(Backend::AntiLag2);' "$R"
grep -q 'value.push_back(Backend::XeLL);' "$R"

echo "PASS Anti-Lag 2 D3D12 output audit"
