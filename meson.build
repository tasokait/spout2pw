project('spout2pw', ['c','cpp'],
    meson_version: '>=1.1'
)

if not meson.is_cross_build()
    error('must be invoked with "meson setup --cross-file=./x86_64-w64-mingw32.txt"')
endif

message('Checking wine PE library path...')
r = run_command(join_paths(meson.project_source_root(), 'tools/get_wine_path.sh'),
    'x86_64-w64-mingw32', '^/.*/libwinecrt0.a$', check: true)
out = r.stdout().strip().split('\n')[0]
if out.endswith('libwinecrt0.a')
    wine_lib_paths = [out.substring(0, -13)]
    message('Wine PE library paths: ' + ' '.join(wine_lib_paths))
else
    error('Wine library path not found')
endif

message('Checking wine UNIX library path...')
r = run_command(join_paths(meson.project_source_root(), 'tools/get_wine_path.sh'),
    '', '^/.*/libwinecrt0.a$', check: true)
out = r.stdout().strip().split('\n')[0]
if out.endswith('libwinecrt0.a')
    unix_lib_paths = [out.substring(0, -13)]
    message('Wine UNIX library paths: ' + ' '.join(unix_lib_paths))
else
    error('Wine library path not found')
endif

message('Checking wine PE include paths...')
r = run_command(join_paths(meson.project_source_root(), 'tools/get_wine_path.sh'),
    'x86_64-w64-mingw32', '^-i(system|dirafter)/', check: true)
wine_include_args = r.stdout().strip().split('\n')
if wine_include_args.length() > 0
    message('Wine PE include args: ' + ' '.join(wine_include_args))
else
    error('Wine include args not found')
endif

message('Checking wine UNIX include paths...')
r = run_command(join_paths(meson.project_source_root(), 'tools/get_wine_path.sh'),
    '', '^-i(system|dirafter)/', check: true)
unix_include_args = r.stdout().strip().split('\n')
if unix_include_args.length() > 0
    message('Wine UNIX include args: ' + ' '.join(unix_include_args))
else
    error('Wine include args not found')
endif

c = meson.get_compiler('c')

spoutdxtoc_proj = subproject('spoutdxtoc',
  default_options: [
    'static=true',
  ],
)
spoutdxtoc_dep = spoutdxtoc_proj.get_variable('spoutdxtoc_dep')

ifpwstatic = get_option('libpipewire_static')

libfunnel_proj = subproject(
  'libfunnel',
  default_options: [
    'native=true',
    'test_apps=false',
    'egl=false',
    'default_library=static',
    'static_pipewire=' + ifpwstatic.to_string(),
  ]
)

funnel = libfunnel_proj.get_variable('funnel')
funnel_vk = libfunnel_proj.get_variable('funnel_vk')

windows_src = [
  'src/spout2pw.c',
]

unix_src = [
  'src/spout2pw_unix.c',
]

windows_compiler_args = [
    '-Wno-error', '-std=c99', '-mcmodel=small',
    '-fno-omit-frame-pointer', '-O2', '-march=nocona',
    '-mtune=core-avx2', '-mfpmath=sse', '-D__WINE_PE_BUILD',
    '-mcx16', '-fno-strict-aliasing', '-ffunction-sections'
] + wine_include_args

windows_link_args = [
  '-static-libgcc',
  '-static-libstdc++',
  '-static',
  # Force UCRT as the primary CRT. Older mingw-w64 toolchains (e.g. Ubuntu
  # 24.04's mingw-w64 11.0.1 / GCC 13-win32) default to msvcrt startup, which
  # pulls msvcrt.dll into the PE import directory and crashes under Steam/Proton
  # before WinMain runs. The stock collect2 `*libgcc:` spec unconditionally
  # injects `-lmsvcrt` via pass-through, so a plain -lucrtbase cannot win.
  # Instead we ship a GCC specs override (tools/mingw-ucrt.specs) that swaps
  # `-lmsvcrt` for `-lucrtbase` in the libgcc link sequence, eliminating the
  # msvcrt import entry entirely. -lucrtbase resolves via the cross-compiler's
  # default library path on every target toolchain.
  '--specs=' + join_paths(meson.project_source_root(), 'tools/mingw-ucrt.specs'),
  # Prune unused CRT sections.
  '-Wl,--gc-sections',
]
unix_compiler_args = [ '-DWINE_UNIX_LIB' ] + unix_include_args

unix_link_args = ifpwstatic ? [
        '-lm',
        '-l:ntdll.so',
        '-Wl,--whole-archive',
        '-Wl,' + get_option('libpipewire_static_lib'),
        '-Wl,--no-whole-archive',
    ]:[
        '-lm',
        '-l:ntdll.so',
        '-Wl,--no-as-needed',
        '-lwinecrt0',
        '-Wl,--as-needed',
    ]
pipewire = ifpwstatic ? dependency('libpipewire-static-0.3', native: true, static: true) : dependency('libpipewire-0.3', native: true, static: false)

windows_deps = [
  c.find_library('d3d11', dirs: wine_lib_paths),
  c.find_library('ntdll', dirs: wine_lib_paths),
  c.find_library('winecrt0', dirs: wine_lib_paths),
  spoutdxtoc_dep,
]

# (UCRT is now forced via -Wl,-Bstatic -lucrtbase ... in windows_link_args above)

vulkan = dependency('vulkan', native: true)
spa = dependency('libspa-0.2', native: true)

unix_deps = [
  funnel,
  funnel_vk,
  vulkan,
  pipewire,
  spa,
]

foreach p : unix_lib_paths
  unix_link_args += ['-L' + p]
endforeach

spout2pw_bin = executable(
  'spout2pw',
  windows_src,
  name_prefix: '',
  c_args: windows_compiler_args,
  link_args: windows_link_args,
  dependencies: [windows_deps],
  win_subsystem: 'console',
)

# Minimal DLL with DllMain that initializes the Wine unixlib handle.
# This DLL is loaded as LO_BUILTIN (via the placeholder marker) so that
# __wine_init_unix_call() succeeds from its DllMain, setting the global
# __wine_unixlib_handle used by spout2pw.exe's ServiceMain.
spout2pw_dll_src = [
  'src/spout2pw_dll_main.c',
]

spout2pw_dll = shared_library(
  'spout2pw_dll',
  spout2pw_dll_src,
  name_prefix: '',
  c_args: windows_compiler_args,
  link_args: windows_link_args,
  dependencies: [
    c.find_library('ntdll', dirs: wine_lib_paths),
    c.find_library('winecrt0', dirs: wine_lib_paths),
  ],
  install: false,
)

unixlib = shared_library(
  'spout2pw_unixlib',
  unix_src,
  name_prefix: '',
  c_args: unix_compiler_args,
  link_args: unix_link_args,
  dependencies: unix_deps,
  native: true,
)

fs = import('fs')

meson.add_install_script('tools/package.sh')
