#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
base="$root/src/low_latency_tech/low_latency_tech.h"
vf="$root/src/low_latency_tech/ll_vulkanflex.h"
ll="$root/src/low_latency.cpp"
d3d="$root/src/low_latency_d3d.cpp"

grep -Fq 'virtual void observe_canonical_frame_start(uint64_t frame_id)' "$base"
grep -Fq 'virtual void observe_canonical_marker(const MarkerParams* marker)' "$base"
grep -Fq 'void observe_canonical_frame_start(uint64_t frame_id) override;' "$vf"
grep -Fq 'void observe_canonical_marker(const MarkerParams* marker) override;' "$vf"
grep -Fq 'observer->observe_canonical_frame_start' "$ll"
grep -Fq 'observer->observe_canonical_marker' "$d3d"

echo 'PASS: canonical observer interface is polymorphic and wired through LowLatencyTech'
