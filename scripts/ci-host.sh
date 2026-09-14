#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

mapfile -t checks < <(
    find scripts -maxdepth 1 -type f \
        \( -name 'audit-*.sh' -o -name 'test-*.sh' \) \
        ! -name 'ci-host.sh' \
        -print | LC_ALL=C sort
)

if [ "${#checks[@]}" -eq 0 ]; then
    echo 'ERROR: no host audit/test scripts discovered' >&2
    exit 1
fi

failed=0
start="$(date +%s)"

echo "Running ${#checks[@]} Vulkanized-Fakenvapi host checks"

for check in "${checks[@]}"; do
    printf '\n==> %s\n' "$check"
    if ! bash "$check"; then
        echo "FAIL: $check" >&2
        failed=$((failed + 1))
    fi
done

elapsed=$(( $(date +%s) - start ))
printf '\nHost qualification complete: total=%d failed=%d elapsed=%ss\n' \
    "${#checks[@]}" "$failed" "$elapsed"

if [ "$failed" -ne 0 ]; then
    exit 1
fi
