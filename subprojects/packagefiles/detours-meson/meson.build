project(
    'detours',
    'cpp',
    version: '4.0.1',
    default_options: [
        'cpp_std=c++17',
        'warning_level=0'
    ]
)

cpp = meson.get_compiler('cpp')

# A normal Meson subproject inherits the parent's host machine/toolchain.
# If this fires, the parent build directory was configured natively instead of
# with the Windows MinGW cross file.  Failing here is intentional and much more
# useful than a later `windows.h: No such file or directory` compiler error.
if host_machine.system() != 'windows'
    error(
        'Detours inherited a non-Windows host compiler. ' +
        'Recreate the parent build directory with the Vulkanized-Fakenvapi ' +
        'Windows cross file.'
    )
endif

detours_include = include_directories('src')

# Vulkanized-Fakenvapi only uses the core transaction/attach API.  The core
# implementation depends on DetourCopyInstruction from disasm.cpp.  Upstream
# disasm.cpp also calls DetourGetModuleSize(), whose implementation lives in
# modules.cpp, so those three translation units form the minimal link-complete
# Detours subset for Vulkanized-Fakenvapi.  Image/offline tooling and the
# CreateProcess helper remain excluded.
detours_sources = files(
    'src/detours.cpp',
    'src/disasm.cpp',
    'src/modules.cpp'
)

detours_args = cpp.get_supported_arguments([
    '-DWIN32_LEAN_AND_MEAN',
    '-D_WIN32_WINNT=0x0501',
    '-Wno-unknown-pragmas',
    '-Wno-unused-parameter',
    '-Wno-cast-function-type'
])

detours_library = static_library(
    'detours',
    detours_sources,
    include_directories: detours_include,
    cpp_args: detours_args,
    install: false
)

detours_dep = declare_dependency(
    link_with: detours_library,
    include_directories: detours_include
)
