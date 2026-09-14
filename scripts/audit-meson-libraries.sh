#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

matches="$({
  grep -RInE --include='meson.build' --include='*.build' \
    '(^|[^[:alnum:]_])(find_library|has_library)[[:space:]]*\(' "$root" || true
} | grep -Ev '^[^:]+:[0-9]+:[[:space:]]*#' || true)"

if [[ -n "$matches" ]]; then
    printf '%s\n' "$matches"
    echo >&2
    echo "ERROR: Meson compiler library probing is forbidden for Vulkanized-Fakenvapi." >&2
    echo "Use declare_dependency(link_args: ['-l<name>']) for MinGW system libraries." >&2
    exit 1
fi

echo "PASS: no Meson find_library()/has_library() probes found"
