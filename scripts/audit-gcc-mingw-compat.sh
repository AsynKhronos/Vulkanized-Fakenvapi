#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail=0

# Project code must include NVAPI through the MinGW compatibility wrapper.
while IFS= read -r line; do
    case "$line" in
        *"include/nvapi_mingw_compat.h:"*) ;;
        *) echo "ERROR: direct nvapi.h include: $line"; fail=1 ;;
    esac
done < <(grep -RIn --exclude-dir=.git -E '#[[:space:]]*include[[:space:]]*[<"]nvapi\.h[>"]' "$root" || true)


# The wrapper must clean up NVIDIA's temporary SAL macros before returning to
# project/STL headers. Keeping __NVAPI_EMPTY_SAL defined after nvapi.h causes
# identifiers such as libstdc++'s __in/__out to be macro-expanded away.
wrapper="$root/include/nvapi_mingw_compat.h"
if ! grep -q '#include <nvapi_lite_salend.h>' "$wrapper"; then
    echo "ERROR: NVAPI wrapper does not perform the final SAL cleanup pass."
    fail=1
fi
if ! grep -q '#undef __NVAPI_EMPTY_SAL' "$wrapper"; then
    echo "ERROR: NVAPI wrapper leaves __NVAPI_EMPTY_SAL enabled."
    fail=1
fi


# Project source must not depend on NVAPI's temporary SAL annotation macros.
# They exist only while nvapi.h is parsed by nvapi_mingw_compat.h.
if grep -RInE '\b__(in|out|inout)\b' "$root/src"; then
    echo "ERROR: NVAPI SAL annotations leaked into project source. Remove them from definitions."
    fail=1
fi

if grep -RIn --exclude-dir=.git '_ReturnAddress' "$root/src"; then
    echo "ERROR: MSVC-only _ReturnAddress() remains in src/."
    fail=1
fi

if grep -qE '^#if \(_MSC_VER < 1299\)' "$root/external/detours/include/detours.h"; then
    echo "ERROR: Detours still treats undefined _MSC_VER as an old MSVC compiler."
    fail=1
fi

if ! grep -q '#include <iomanip>' "$root/src/log.h" && ! grep -q '#include <iomanip>' "$root/src/log.cpp"; then
    echo "ERROR: logging code uses std::setprecision without <iomanip>."
    fail=1
fi

if (( fail != 0 )); then
    exit 1
fi


# VK_NULL_HANDLE may be nullptr in modern Vulkan headers.  `auto expected =
# VK_NULL_HANDLE` therefore deduces std::nullptr_t and is invalid as the
# lvalue expected argument of std::atomic<VkHandle>::compare_exchange_*().
if grep -R -n --include='*.cpp' --include='*.h' \
    -E 'auto[[:space:]]+expected[[:space:]]*=[[:space:]]*VK_NULL_HANDLE[[:space:]]*;' \
    "$root/src" >/dev/null; then
    echo "FAIL: untyped VK_NULL_HANDLE compare-exchange expected value found" >&2
    exit 1
fi

echo "PASS: GCC/MinGW compatibility source audit"
