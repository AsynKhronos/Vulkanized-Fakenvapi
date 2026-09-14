#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

policy_files=(
  "$ROOT/src/runtime_policy.h"
  "$ROOT/src/runtime_policy.cpp"
  "$ROOT/src/policy_engine.h"
  "$ROOT/src/policy_engine.cpp"
  "$ROOT/src/input_arbiter.h"
  "$ROOT/src/input_arbiter.cpp"
  "$ROOT/src/hybrid_fusion.h"
  "$ROOT/src/hybrid_fusion.cpp"
)

if grep -nE 'std::(map|unordered_map|vector|list|deque|function|shared_ptr)' "${policy_files[@]}"; then
  echo "FAIL: dynamic/heavy STL container leaked into common policy path" >&2
  exit 1
fi

if ! grep -q 'std::atomic<const policy::RuntimePolicySnapshot\*>' "$ROOT/src/config.h"; then
  echo "FAIL: runtime snapshot is not atomically published as a stable pointer" >&2
  exit 1
fi

if ! grep -q 'compute_routing_signature' "$ROOT/src/runtime_policy.cpp"; then
  echo "FAIL: routing signature implementation missing" >&2
  exit 1
fi

echo "PASS: policy hot-path architecture audit"
