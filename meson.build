project(
    'vulkanized-fakenvapi',
    'cpp',
    version: '0.9.0',
    default_options: [
        'cpp_std=c++23',
        'warning_level=2'
    ]
)

cpp = meson.get_compiler('cpp')

# This project produces Windows NVAPI DLLs.  On Linux it must therefore be
# configured with one of the MinGW cross files at the initial Meson setup.
# A native Linux build would otherwise fail later in Windows-only sources or
# dependencies with misleading missing-header errors.
if host_machine.system() != 'windows'
    error(
        'Vulkanized-Fakenvapi must target Windows. ' +
        'On Linux create a fresh build directory with ' +
        '--cross-file build-win64.txt (or build-win32.txt). ' +
        'Meson cannot convert an already configured native build tree into ' +
        'a cross build by reconfigure alone.'
    )
endif

# Do not use compiler.find_library()/has_library() for MinGW system libraries.
# With some cross toolchains Meson probes the wrong library search model even
# though the linker can resolve the normal -l form correctly.
lib_dxgi = declare_dependency(link_args: ['-ldxgi'])

link_args = [
    '-static',
    '-static-libgcc',
    '-static-libstdc++',
    '-Wl,--file-alignment=4096'
]

compiler_args = [
    '-Wno-unused-parameter',
    '-D_CRT_SECURE_NO_WARNINGS',
    '-D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR'
]

spdlog_headers = include_directories('./external/spdlog/include')
xell_headers = include_directories('./external/xell/inc')
vulkan_headers = include_directories('./external/vulkan/include')
nvapi_headers = include_directories('./external/nvapi')
magic_enum_headers = include_directories('./external/magic_enum')
fakenvapi_headers = include_directories('./include')

# Never link the vendored MSVC detours.lib into a MinGW build.  C++ name
# mangling and MSVC runtime/GS references make that binary ABI-incompatible
# with GCC.  Build the pinned Detours sources with the host MinGW compiler.
detours_project = subproject('detours')
lib_detours = detours_project.get_variable('detours_dep')

windows = import('windows')

rc_file = configure_file(
    input: 'resource.rc.in',
    output: 'resource.rc',
    configuration: {
        'FILEVERSION': '0,9,0,0',
    }
)

rc = windows.compile_resources(rc_file)

fakenvapi_version = vcs_tag(
    command: ['git', 'describe', '--always', '--tags', '--dirty=-dirty'],
    fallback: meson.project_version(),
    input: 'version.h.in',
    output: 'version.h'
)

add_project_arguments(cpp.get_supported_arguments(compiler_args), language: 'cpp')
add_project_link_arguments(cpp.get_supported_link_arguments(link_args), language: 'cpp')

# Fail early if somebody reintroduces Meson compiler library probes. They are
# deliberately avoided in this project because of MinGW cross-build behavior.
if meson.is_cross_build() and host_machine.system() == 'windows'
    message('Vulkanized-Fakenvapi compiler: ' + cpp.get_id())
    message('Vulkanized-Fakenvapi target: ' + target_machine.cpu_family() + '-windows')
endif

cpu_family = target_machine.cpu_family()
if cpu_family == 'x86_64'
    target_suffix = '64'
else
    target_suffix = ''
endif

subdir('src')
