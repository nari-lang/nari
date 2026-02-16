project('interpreter', 'cpp', default_options: ['cpp_std=c++20'], version: '0.0.1')

add_project_arguments([], language: 'cpp')

python3 = find_program('python3', required: true)

embed_script = meson.current_source_dir() / 'tools' / 'embed_resource.py'
stdlib_input = meson.current_source_dir() / 'src' / 'stdlib' / 'stdlib.nari'
generated_dir = meson.current_build_dir() / 'generated'
generated_cpp_name = 'embedded_stdlib.cpp'
generated_cpp = generated_dir / generated_cpp_name

embed_target = custom_target('embed-stdlib',
  input : stdlib_input,
  output : generated_cpp_name,
  command : [ python3, embed_script, stdlib_input, '@OUTPUT@', 'nari_embedded_stdlib' ],
  build_by_default : true,
)

sources = [
  'src/interpreter.cpp',
  'src/parser.cpp',
  'src/runtime.cpp',
  'src/builtins.cpp',
  'src/ffi.cpp',
  'src/core_types.cpp',
  'src/gc.cpp',
  embed_target,
]

inc_dirs = include_directories('thirdparty')

cc = meson.get_compiler('cpp')

# Detect target system for dependency paths
is_windows = host_machine.system() == 'windows'
is_emscripten = host_machine.system() == 'emscripten'
dep_suffix = is_windows ? '-windows' : '-linux'

# Add Windows-specific stub for ___chkstk_ms
if is_windows
  sources += ['src/chkstk_stub.cpp']
endif

# Platform-specific dependencies
all_deps = []

# OpenSSL - not needed for emscripten
if not is_emscripten
  if is_windows
    openssl_prefix = is_windows ? 'thirdparty/openssl/build-windows' : 'thirdparty/openssl/build-linux'
    openssl_inc = include_directories(openssl_prefix / 'include')

    # Try lib64 first, then lib (Windows uses lib)
    openssl_lib_dir = (openssl_prefix / 'lib64')

    ssl_lib = cc.find_library(
      'ssl',
      dirs: [meson.project_source_root() / openssl_lib_dir],
      static: true,
      required: true
    )

    crypto_lib = cc.find_library(
      'crypto',
      dirs: [meson.project_source_root() / openssl_lib_dir],
      static: true,
      required: true
    )

    openssl_dep = declare_dependency(
      include_directories: openssl_inc,
      dependencies: [ssl_lib, crypto_lib],
    )
    
    all_deps += [openssl_dep]
  else
    # Non-Windows: Try system libraries first, fall back to static
    ssl_lib = cc.find_library('ssl', required: false)
    crypto_lib = cc.find_library('crypto', required: false)
    
    if ssl_lib.found() and crypto_lib.found()
      openssl_dep = declare_dependency(
        dependencies: [ssl_lib, crypto_lib],
      )
    else
      # Fall back to static libraries
      openssl_lib_dir = meson.current_source_dir() / 'thirdparty' / 'openssl' / 'build-linux' / 'lib'
      openssl_inc_dir = meson.current_source_dir() / 'thirdparty' / 'openssl' / 'build-linux' / 'include'
      
      ssl_lib = cc.find_library('ssl', dirs: [openssl_lib_dir], static: true, required: true)
      crypto_lib = cc.find_library('crypto', dirs: [openssl_lib_dir], static: true, required: true)
      
      openssl_dep = declare_dependency(
        dependencies: [ssl_lib, crypto_lib],
        include_directories: include_directories(openssl_inc_dir)
      )
    endif
  endif

  all_deps += [openssl_dep]
endif

# libffi - not available for emscripten
if not is_emscripten
  if is_windows
    ffi_build_dir = is_windows ? 'thirdparty/libffi-3.5.2/build-windows' : 'thirdparty/libffi-3.5.2/build'
    ffi_inc = include_directories(ffi_build_dir / 'include')
    ffi_lib = cc.find_library(
      'ffi',
      dirs: [meson.project_source_root() / ffi_build_dir / 'lib'],
      static: true,
      required: true
    )
    ffi_dep = declare_dependency(
      include_directories: ffi_inc,
      dependencies: [ffi_lib],
    )
  else
    # Non-Windows: Try system library first, fall back to static
    ffi_lib = cc.find_library('ffi', required: false)
    
    if ffi_lib.found()
      ffi_dep = declare_dependency(
        dependencies: [ffi_lib],
      )
    else
      # Fall back to static library
      ffi_lib_dir = meson.current_source_dir() / 'thirdparty' / 'libffi-3.5.2' / 'build' / 'lib'
      ffi_inc_dir = meson.current_source_dir() / 'thirdparty' / 'libffi-3.5.2' / 'build' / 'include'
      
      ffi_lib = cc.find_library('ffi', dirs: [ffi_lib_dir], static: true, required: true)
      ffi_dep = declare_dependency(
        dependencies: [ffi_lib],
        include_directories: include_directories(ffi_inc_dir)
      )
    endif
  endif

  all_deps += [ffi_dep]
endif

# Platform-specific system libraries
if is_windows
  # Windows doesn't need dl or separate c++ library
  platform_deps = []
elif is_emscripten
  # Emscripten doesn't need these either
  platform_deps = []
else
  dl_dep = cc.find_library('dl', required: true)
  cxx_dep = cc.find_library('c++', required: true)
  platform_deps = [dl_dep, cxx_dep]
endif

all_deps += platform_deps

# Emscripten-specific link arguments
link_args = []
if is_emscripten
  tests_dir = meson.project_source_root() / 'tests'
  shell_file = meson.project_source_root() / 'shell.html'
  link_args = [
    '--preload-file', tests_dir + '@/tests/',
    '--shell-file', shell_file
  ]
endif

executable(
  'interpreter',
  sources,
  include_directories: inc_dirs,
  dependencies: all_deps,
  link_args: link_args,
  install: true,
)
