#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

hooks="$root/src/openxr_hooks.cpp"
timeline="$root/src/openxr_timeline.cpp"
low="$root/src/low_latency.cpp"
vkhooks="$root/src/vulkan_hooks.cpp"
meson="$root/src/meson.build"

for needle in \
  'xrGetInstanceProcAddr' \
  'xrWaitFrame' \
  'xrBeginFrame' \
  'xrEndFrame' \
  'xrDestroySession' \
  'openxr_loader.dll'; do
  grep -q "$needle" "$hooks"
done

grep -q 'try_hook_loaded_openxr' "$vkhooks"
grep -q 'openxr_hooks.cpp' "$meson"
grep -q 'openxr_timeline.cpp' "$meson"
grep -q 'timing_owner=openxr-runtime' "$low"
grep -q 'render_sequence_watermark' "$timeline"

# R4.1 is observe-only. It must not add a sleep/wait/present/submit path or
# mutate the runtime's predicted/submitted XrTime values.
if grep -Eq 'Sleep\(|sleep_for|sleep_until|WaitForSingleObject|vkQueueSubmit|vkWaitForPresent|xrWaitFrame\s*\(' "$timeline"; then
  echo 'FAIL: XRFlex timeline contains an execution wait/submission path' >&2
  exit 1
fi
if grep -Eq 'predictedDisplayTime\s*=|predictedDisplayPeriod\s*=|displayTime\s*=' "$hooks"; then
  echo 'FAIL: OpenXR hook mutates runtime/application timing fields' >&2
  exit 1
fi

# The real xrWaitFrame is invoked before observation, preserving runtime timing
# ownership. The GIPA hook must return local frame observers for cached tables.
python - "$hooks" <<'PY'
from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text()
body=s.rsplit('OpenXRHooks::hkxrWaitFrame',1)[1].split('OpenXRHooks::hkxrBeginFrame',1)[0]
assert body.index('const auto result = fn(') < body.index('OpenXROnWaitFrame')
gipa=s.rsplit('OpenXRHooks::hkxrGetInstanceProcAddr',1)[1].split('OpenXRHooks::hkxrWaitFrame',1)[0]
assert 'publish_indirect(name, *function)' in gipa
assert '*function = hook' in gipa
PY

echo 'OpenXR/XRFlex audit: PASS'
