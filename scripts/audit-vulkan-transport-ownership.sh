#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VH="$ROOT/src/vulkan_hooks.cpp"

require() { grep -Fq "$1" "$2" || { echo "FAIL missing '$1' in $2" >&2; exit 1; }; }
reject() { if grep -Fq "$1" "$2"; then echo "FAIL forbidden '$1' in $2" >&2; exit 1; fi; }

# Translation-stack classification must be explicit and independent of whether
# vulkan-1.dll has already loaded.
require "stack_has_translation_layer" "$VH"
require "VulkanStackLayer::Vkd3dProton" "$VH"
require "VulkanStackLayer::Dxvk" "$VH"
require "Transport classification must work even before vulkan-1.dll is loaded" "$VH"

# native_only is a transport ownership rule, not merely an on_acquire filter.
require "translation_stack && snapshot.vulkanflex.only_native_vulkan" "$VH"
require "g_translation_bypass.store(true" "$VH"
require "Vulkan transport bypass:" "$VH"
require "Skipping Vulkan core/WSI detours" "$VH"

# Delayed loader discovery must re-enter initialize(), otherwise it bypasses the
# coexistence/transport classifier and can hook VKD3D/DXVK later in startup.
python - "$VH" <<'PY2'
from pathlib import Path
import re, sys
s=Path(sys.argv[1]).read_text()
m=re.search(r'void try_hook_loaded_vulkan\(\) \{(.*?)\n\}', s, re.S)
if not m:
    raise SystemExit('FAIL: try_hook_loaded_vulkan not found')
body=m.group(1)
if 'VulkanHooks::initialize(module);' not in body:
    raise SystemExit('FAIL: delayed loader does not revalidate through initialize()')
if 'VulkanHooks::hook_vulkan(module);' in body:
    raise SystemExit('FAIL: delayed loader bypasses transport policy via direct hook_vulkan()')

# The native_only translation bypass must execute before the call that installs
# Vulkan detours.
pos_bypass=s.find('translation_stack && snapshot.vulkanflex.only_native_vulkan')
pos_hook=s.find('hook_vulkan(vulkan_module);', pos_bypass)
if pos_bypass < 0 or pos_hook < 0 or pos_bypass > pos_hook:
    raise SystemExit('FAIL: translation bypass is not ordered before hook installation')
PY2

echo "PASS: Vulkan transport ownership/native-only/delayed-loader audit"
