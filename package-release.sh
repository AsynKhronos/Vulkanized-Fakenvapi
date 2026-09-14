#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <version> <destination-directory>" >&2
    exit 2
fi

VERSION="$1"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST_ROOT="$2"

mkdir -p "$DEST_ROOT"
DEST_ROOT="$(realpath "$DEST_ROOT")"
PACKAGE_DIR="$DEST_ROOT/Vulkanized-Fakenvapi-$VERSION"

if [ -e "$PACKAGE_DIR" ]; then
    echo "ERROR: release directory already exists: $PACKAGE_DIR" >&2
    exit 1
fi

for tool in meson ninja; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: required tool not found: $tool" >&2
        exit 1
    }
done

BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/vulkanized-fakenvapi-release.XXXXXX")"
trap 'rm -rf "$BUILD_ROOT"' EXIT

mkdir -p "$PACKAGE_DIR"

# Detours is pinned by subprojects/detours.wrap and source-built with the same
# MinGW target toolchain as the DLL. Download once before configuring either
# architecture.
(
    cd "$SRC_DIR"
    meson subprojects download detours
)

build_arch() {
    local bits="$1"
    local crossfile="$SRC_DIR/build-win${bits}.txt"
    local builddir="$BUILD_ROOT/build.${bits}"

    meson setup \
        --cross-file "$crossfile" \
        --buildtype release \
        --prefix "$PACKAGE_DIR" \
        --strip \
        --bindir "x${bits}" \
        --libdir "x${bits}" \
        "$builddir" \
        "$SRC_DIR"

    ninja -C "$builddir"
    meson install -C "$builddir"
}

build_arch 64
build_arch 32

install -m 0644 "$SRC_DIR/fakenvapi.ini" "$PACKAGE_DIR/fakenvapi.ini"
install -m 0644 "$SRC_DIR/README.md" "$PACKAGE_DIR/README.md"
install -m 0644 "$SRC_DIR/LICENSE" "$PACKAGE_DIR/LICENSE"
install -m 0644 "$SRC_DIR/THIRD_PARTY_NOTICES.md" "$PACKAGE_DIR/THIRD_PARTY_NOTICES.md"
install -m 0644 "$SRC_DIR/CHANGELOG.md" "$PACKAGE_DIR/CHANGELOG.md"

if [ -d "$SRC_DIR/LICENSES" ]; then
    cp -a "$SRC_DIR/LICENSES" "$PACKAGE_DIR/LICENSES"
fi

{
    echo "Vulkanized-Fakenvapi $VERSION"
    if git -C "$SRC_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        echo "commit=$(git -C "$SRC_DIR" rev-parse HEAD)"
        if ! git -C "$SRC_DIR" diff --quiet --ignore-submodules -- 2>/dev/null; then
            echo "working_tree=dirty"
        else
            echo "working_tree=clean"
        fi
    else
        echo "commit=unavailable"
    fi
} > "$PACKAGE_DIR/BUILD-INFO.txt"

printf 'Release package staged at: %s\n' "$PACKAGE_DIR"
find "$PACKAGE_DIR" -maxdepth 2 -type f -printf '%P\n' | LC_ALL=C sort
