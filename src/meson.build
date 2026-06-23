py = import('python').find_installation(required : true)
cpp = meson.get_compiler('cpp')

host_system = host_machine.system()
host_cpu_family = host_machine.cpu_family()
is_windows = host_system == 'windows'
is_darwin = host_system == 'darwin'
is_linux = host_system == 'linux'
is_msvc = cpp.get_id() == 'msvc'

if not is_windows and not is_darwin and not is_linux
  error('openQ4-GameLibs Meson build supports Windows/MSVC, macOS/Clang, and Linux/GCC+Clang.')
endif

if is_windows and not is_msvc
  error('openQ4-GameLibs Windows builds require MSVC cl.exe.')
endif

if is_darwin and cpp.get_id() != 'clang'
  error('openQ4-GameLibs macOS builds require Clang.')
endif

if is_windows and not ['x86', 'x86_64'].contains(host_cpu_family)
  error('openQ4-GameLibs Windows builds currently support x86 and x64 MSVC toolchains.')
endif

if is_darwin and not ['x86_64', 'aarch64'].contains(host_cpu_family)
  error('openQ4-GameLibs macOS builds currently support x64 and arm64 Clang toolchains.')
endif

if is_linux and not ['x86_64', 'aarch64'].contains(host_cpu_family)
  error('openQ4-GameLibs Linux builds currently support x64 and arm64 toolchains.')
endif

if host_cpu_family == 'x86_64'
  game_arch = 'x64'
elif host_cpu_family == 'x86'
  game_arch = 'x86'
elif host_cpu_family == 'aarch64'
  game_arch = 'arm64'
else
  error('Unsupported game module architecture: ' + host_cpu_family)
endif

sp_module_name = 'game-sp_' + game_arch
mp_module_name = 'game-mp_' + game_arch

source_lister = join_paths(meson.current_source_dir(), 'buildscripts', 'list_sources.py')
source_root = meson.current_source_dir()
release_like = ['release', 'debugoptimized', 'minsize'].contains(get_option('buildtype'))
source_inc = include_directories('.')

common_cpp_args = []
if is_msvc
  common_cpp_args += [
    '/std:c++17',
    '/W4',
    '/FS',
    '/source-charset:windows-1252',
    '/execution-charset:windows-1252',
    '/Zc:strictStrings-',
    '/Zc:ternary-',
    '/wd4091',
    '/wd4456',
    '/wd4457',
    '/wd4458',
    '/wd4459',
    '/wd4595',
    '/wd4996',
    '/wd4702',
    '/wd4740',
    '/wd4750',
  ]
else
  if not cpp.has_argument('-std=c++17')
    error('openQ4-GameLibs non-MSVC builds require C++17 compiler support.')
  endif
  common_cpp_args += ['-std=c++17']

  foreach compiler_arg : [
    '-fpermissive',
    '-finput-charset=windows-1252',
    '-Wno-invalid-source-encoding',
    '-Wno-deprecated-declarations',
    '-Wno-writable-strings',
  ]
    if cpp.has_argument(compiler_arg)
      common_cpp_args += compiler_arg
    endif
  endforeach
endif

game_forced_include_args = is_msvc ? [
  '/FI' + join_paths(meson.current_source_dir(), 'idlib', 'precompiled.h')
] : [
  '-include',
  join_paths(meson.current_source_dir(), 'idlib', 'precompiled.h')
]

mssdk_include = get_option('mssdk_include')
if mssdk_include != ''
  if is_msvc
    common_cpp_args += ['/I' + join_paths(meson.current_source_dir(), mssdk_include)]
  else
    common_cpp_args += ['-I' + join_paths(meson.current_source_dir(), mssdk_include)]
  endif
endif

common_defines = [
  'Q4SDK',
  'USE_OPENAL',
]

if is_windows
  common_defines += [
    '_WINDOWS',
    'WIN32',
  ]
elif is_darwin
  common_defines += ['MACOS_X=1']
elif is_linux
  common_defines += ['LINUX']
endif

if release_like
  common_defines += ['NDEBUG']
else
  common_defines += ['_DEBUG']
endif

inline_debug_enabled = get_option('inline_debug') and not release_like
if inline_debug_enabled
  common_defines += ['_INLINEDEBUG']
  if is_msvc
    common_cpp_args += ['/Ob1']
  endif
endif

define_arg_prefix = is_msvc ? '/D' : '-D'
common_define_args = []
foreach define : common_defines
  common_define_args += define_arg_prefix + define
endforeach
game_dll_define_arg = define_arg_prefix + 'GAME_DLL'
game_mpapi_define_arg = define_arg_prefix + 'GAME_MPAPI'

idlib_excludes = [
  'idlib/math/Simd_AltiVec.cpp',
]

if host_cpu_family != 'x86'
  idlib_excludes += [
    'idlib/math/Simd_3DNow.cpp',
    'idlib/math/Simd_MMX.cpp',
    'idlib/math/Simd_SSE.cpp',
    'idlib/math/Simd_SSE2.cpp',
    'idlib/math/Simd_SSE3.cpp',
  ]
endif

idlib_source_output = run_command(
  py,
  source_lister,
  source_root,
  'idlib',
  idlib_excludes,
  check : true,
).stdout().strip().split('\n')

idlib_sources = []
foreach source_line : idlib_source_output
  rel = source_line.strip()
  if rel != ''
    idlib_sources += files(rel)
  endif
endforeach

idlib = static_library(
  'idLib',
  idlib_sources,
  include_directories : source_inc,
  cpp_args : common_cpp_args + common_define_args + [game_dll_define_arg],
  pic : not is_windows,
  install : false,
)

_install_root_override = get_option('install_root_override')
install_game_dir = _install_root_override != '' ? _install_root_override / 'baseoq4' : ''

module_link_args = []
if is_windows
  module_link_args = [
    '/FIXED:NO',
    '/LARGEADDRESSAWARE',
  ]

  if host_cpu_family == 'x86_64'
    module_link_args += ['/BASE:0x180000000']
  else
    module_link_args += ['/BASE:0x20000000']
  endif

  if release_like
    module_link_args += [
      '/STACK:4194304',
      '/OPT:REF',
      '/OPT:ICF',
      '/MAP',
    ]
  else
    module_link_args += ['/STACK:16000000,16000000']
  endif
elif is_darwin
  foreach link_arg : [
    '-Wl,-dead_strip',
    '-Wl,-headerpad_max_install_names',
  ]
    if cpp.has_link_argument(link_arg)
      module_link_args += link_arg
    endif
  endforeach
elif is_linux
  foreach link_arg : [
    '-Wl,--as-needed',
    '-Wl,-z,noexecstack',
    '-Wl,-z,relro',
    '-Wl,-z,now',
  ]
    if cpp.has_link_argument(link_arg)
      module_link_args += link_arg
    endif
  endforeach
endif

if get_option('build_spgame')
  game_source_output = run_command(
    py,
    source_lister,
    source_root,
    'game',
    'game/Callbacks.cpp',
    'game/gamesys/Callbacks.cpp',
    check : true,
  ).stdout().strip().split('\n')

  game_sources = []
  foreach source_line : game_source_output
    rel = source_line.strip()
    if rel != ''
      game_sources += files(rel)
    endif
  endforeach

  if is_windows
    shared_module(
      sp_module_name,
      game_sources,
      include_directories : source_inc,
      link_with : idlib,
      cpp_args : common_cpp_args + common_define_args + [game_dll_define_arg] + game_forced_include_args,
      link_args : module_link_args,
      vs_module_defs : 'game/game.def',
      name_prefix : '',
      install : false,
    )
  elif is_darwin
    shared_module(
      sp_module_name,
      game_sources,
      include_directories : source_inc,
      link_with : idlib,
      cpp_args : common_cpp_args + common_define_args + [game_dll_define_arg] + game_forced_include_args,
      link_args : module_link_args + ['-Wl,-install_name,@loader_path/' + sp_module_name + '.dylib'],
      name_prefix : '',
      name_suffix : 'dylib',
      install : false,
    )
  elif is_linux
    shared_module(
      sp_module_name,
      game_sources,
      include_directories : source_inc,
      link_with : idlib,
      cpp_args : common_cpp_args + common_define_args + [game_dll_define_arg] + game_forced_include_args,
      link_args : module_link_args,
      name_prefix : '',
      name_suffix : 'so',
      install : install_game_dir != '',
      install_dir : install_game_dir,
    )
  endif
endif

if get_option('build_mpgame')
  mpgame_source_output = run_command(
    py,
    source_lister,
    source_root,
    'mpgame',
    'mpgame/Callbacks.cpp',
    'mpgame/gamesys/Callbacks.cpp',
    check : true,
  ).stdout().strip().split('\n')

  mpgame_sources = []
  foreach source_line : mpgame_source_output
    rel = source_line.strip()
    if rel != ''
      mpgame_sources += files(rel)
    endif
  endforeach

  if is_windows
    shared_module(
      mp_module_name,
      mpgame_sources,
      include_directories : source_inc,
      link_with : idlib,
      cpp_args : common_cpp_args + common_define_args + [
        game_dll_define_arg,
        game_mpapi_define_arg,
      ] + game_forced_include_args,
      link_args : module_link_args,
      vs_module_defs : 'mpgame/mpgame.def',
      name_prefix : '',
      install : false,
    )
  elif is_darwin
    shared_module(
      mp_module_name,
      mpgame_sources,
      include_directories : source_inc,
      link_with : idlib,
      cpp_args : common_cpp_args + common_define_args + [
        game_dll_define_arg,
        game_mpapi_define_arg,
      ] + game_forced_include_args,
      link_args : module_link_args + ['-Wl,-install_name,@loader_path/' + mp_module_name + '.dylib'],
      name_prefix : '',
      name_suffix : 'dylib',
      install : false,
    )
  elif is_linux
    shared_module(
      mp_module_name,
      mpgame_sources,
      include_directories : source_inc,
      link_with : idlib,
      cpp_args : common_cpp_args + common_define_args + [
        game_dll_define_arg,
        game_mpapi_define_arg,
      ] + game_forced_include_args,
      link_args : module_link_args,
      name_prefix : '',
      name_suffix : 'so',
      install : install_game_dir != '',
      install_dir : install_game_dir,
    )
  endif
endif

summary(
  {
    'Host system': host_system,
    'Host cpu family': host_cpu_family,
    'Compiler': cpp.get_id(),
    'Build Type': get_option('buildtype'),
    'Build SP Game': get_option('build_spgame'),
    'Build MP Game': get_option('build_mpgame'),
    'Game Module Arch': game_arch,
    'Install Dir': install_game_dir != '' ? install_game_dir : '(not installing)',
    'Inline Debug': inline_debug_enabled,
    'Legacy Include': mssdk_include == '' ? '(disabled)' : mssdk_include,
  },
  section : 'openQ4-GameLibs',
)
