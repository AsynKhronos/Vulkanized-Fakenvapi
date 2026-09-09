# Building

> This is a conservative Meson-oriented baseline. Update dependency packages to match the actual source tree.

## Requirements

- Meson
- Ninja
- Clang (primary)
- GCC (secondary compatibility target)
- Vulkan headers / development package
- project-specific dependencies required by the source tree

## Configure with Clang

```bash
CC=clang CXX=clang++ meson setup build \
  --buildtype=release \
  -Db_ndebug=true
```

## Compile

```bash
meson compile -C build
```

## Run tests

```bash
meson test -C build --print-errorlogs
```

## Clean rebuild

```bash
rm -rf build
CC=clang CXX=clang++ meson setup build --buildtype=release
meson compile -C build
```

## GCC compatibility build

```bash
rm -rf build-gcc
CC=gcc CXX=g++ meson setup build-gcc --buildtype=release
meson compile -C build-gcc
meson test -C build-gcc --print-errorlogs
```

## Important

Do not add compiler-specific flags merely because they look faster. Use profile-guided measurements and keep
portable release behavior unless a target-specific path has a measurable benefit.
