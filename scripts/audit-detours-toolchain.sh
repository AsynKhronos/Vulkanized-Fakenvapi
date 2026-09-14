#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if grep -RIn --include='meson.build' --include='*.wrap' \
    'external/detours/lib/detours\.lib' . >/tmp/vf-detours-msvc-lib.$$ 2>/dev/null; then
    cat /tmp/vf-detours-msvc-lib.$$
    rm -f /tmp/vf-detours-msvc-lib.$$
    echo 'ERROR: MSVC detours.lib must not be linked by the MinGW build.' >&2
    exit 1
fi
rm -f /tmp/vf-detours-msvc-lib.$$ || true

grep -q '^revision = adb07604aa56508448b95bf037c2a6d0d3b6831a$' subprojects/detours.wrap
grep -q "'src/detours.cpp'" subprojects/packagefiles/detours-meson/meson.build
grep -q "'src/disasm.cpp'" subprojects/packagefiles/detours-meson/meson.build
grep -q "'src/modules.cpp'" subprojects/packagefiles/detours-meson/meson.build

echo 'PASS: Detours is source-built with the MinGW toolchain from the pinned upstream commit.'
