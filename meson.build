project(
    'nari',
    'cpp',
    'c',
    default_options: ['cpp_std=c++17', 'c_std=c11'],
    version: '0.0.1',
)

is_windows = host_machine.system() == 'windows'
is_emscripten = host_machine.system() == 'emscripten'
is_darwin = host_machine.system() == 'darwin'

disable_ffi = get_option('disable_ffi')
disable_http = get_option('disable_http')
disable_jit = get_option('disable_jit')
disable_repl = get_option('disable_repl') or is_emscripten
disable_lsp = get_option('disable_lsp')
target_mcu = get_option('target_mcu')
windows_win7_compat = is_windows and get_option('windows_win7_compat')

# MCU target: force off everything that needs an OS or POSIX threads.
# Any feature flag already explicitly set is left alone
if target_mcu
    disable_ffi = true
    disable_http = true
    disable_jit = true
    disable_repl = true
endif

cc = meson.get_compiler('cpp')
fs = import('fs')

# conan --deployer=direct_deploy puts each package under:
#  <deploy-root>/<pkg>/{include,lib}
# The default deploy root is build/conan/direct_deploy, but callers can
# override it via a native/cross file property to keep multiple builds isolated from each other.
# -Dsystem_deps=true: link distro packages found via pkg-config / default
# linker paths and ignore the Conan deploy folder entirely (for packaging).
system_deps = get_option('system_deps')

conan_deploy = meson.get_external_property('conan_deploy_dir', 'build/conan/direct_deploy')
conan_deploy_abs = meson.project_source_root() / conan_deploy

conan_asmjit_libdir = [conan_deploy_abs / 'asmjit' / 'lib']
conan_mbedtls_libdir = [conan_deploy_abs / 'mbedtls' / 'lib']
conan_zlib_libdir = [conan_deploy_abs / 'zlib' / 'lib']
conan_curl_libdir = [conan_deploy_abs / 'libcurl' / 'lib']
conan_ffi_libdir = [conan_deploy_abs / 'libffi' / 'lib']
conan_replxx_libdir = [conan_deploy_abs / 'replxx' / 'lib']
conan_libarchive_libdir = [conan_deploy_abs / 'libarchive' / 'lib']

# Probe for replxx early so we can add -DDISABLE_REPL to cpp_args if it is unavailable
have_replxx = false
_replxx_lib = disabler()
if not disable_repl and system_deps
    _replxx_sys = cc.find_library('replxx', required: false)
    if _replxx_sys.found() and cc.has_header('replxx.hxx')
        _replxx_lib = _replxx_sys
        have_replxx = true
    else
        message('system replxx not found; REPL disabled (system_deps build)')
        disable_repl = true
    endif
elif not disable_repl
    replxx_candidates = [
        conan_deploy_abs / 'replxx' / 'lib' / 'replxx.lib',
        conan_deploy_abs / 'replxx' / 'lib' / 'replxx-static.lib',
        conan_deploy_abs / 'replxx' / 'lib' / 'libreplxx.lib',
        conan_deploy_abs / 'replxx' / 'lib' / 'libreplxx.a',
    ]

    foreach replxx_path : replxx_candidates
        if fs.exists(replxx_path)
            message('Using replxx from: ' + replxx_path)
            _replxx_lib = declare_dependency(
                compile_args: is_windows ? ['-DREPLXX_STATIC'] : [],
                link_args: [replxx_path],
            )
            have_replxx = true
            break
        endif
    endforeach

    if not have_replxx
        message('replxx not found in Conan deploy folder! REPL will not work.')
        disable_repl = true
    endif
endif

cpp_args = []
link_args = []
if target_mcu
    # NARI_MCU: top-level gate for all MCU-specific code paths.
    # NO_THREADS: forces IOThreadPool to the synchronous stub (no pthreads).
    # DISABLE_PARSER: interpreter accepts only pre-compiled .naric bytecode;
    #  source parsing and the embedded stdlib are not available at runtime.
    cpp_args += [
        '-DNARI_MCU',
        '-DNO_THREADS',
        '-DDISABLE_PARSER',
        '-ffunction-sections',
        '-fdata-sections',
        '-flto',
    ]
    link_args += ['-flto', '-Wl,--gc-sections', '-Wl,-s']
endif
if disable_ffi
    cpp_args += ['-DDISABLE_FFI']
elif is_windows
    cpp_args += ['-DFFI_STATIC_BUILD']
endif
if disable_http
    cpp_args += ['-DDISABLE_HTTP']
endif
if disable_jit
    cpp_args += ['-DDISABLE_JIT']
else
    buildtype = get_option('buildtype')
    if buildtype == 'debug' or buildtype == 'debugoptimized'
        cpp_args += ['-DNARI_ENABLE_GDB_JIT']
    endif
endif
if disable_repl
    cpp_args += ['-DDISABLE_REPL']
endif
if windows_win7_compat
    cpp_args += [
        '-DNARI_WINDOWS_WIN7_COMPAT',
        '-DWINVER=0x0601',
        '-D_WIN32_WINNT=0x0601',
        '-DNTDDI_VERSION=0x06010000',
    ]
endif

add_project_arguments(cpp_args, language: 'cpp')

# TODO: I would be really really happy if someone could make this work with something else instead of python
python3 = find_program('python3', required: true)
embed_script = meson.current_source_dir() / 'tools' / 'embed_resource.py'
embed_std_modules_script = meson.current_source_dir() / 'tools' / 'embed_std_modules.py'
stdlib_dir = meson.current_source_dir() / 'src' / 'stdlib'
prelude_input = stdlib_dir / 'std' / 'prelude.nari'
generated_dir = meson.current_build_dir() / 'generated'
generated_cpp_name = 'embedded_std_modules.cpp'
generated_cpp = generated_dir / generated_cpp_name

# Importable std/* modules. Each entry becomes `import { X } from "std/<name>";`.
# Keep this list in sync with src/stdlib/std/*.nari.
std_module_names = [
    'archive',
    'collections',
    'date',
    'encoding',
    'hash',
    'logger',
    'path',
    'random',
    'regex',
    'url',
    'uuid',
]

std_module_inputs = [prelude_input]
std_module_specs = []
foreach name : std_module_names
    module_path = stdlib_dir / 'std' / (name + '.nari')
    std_module_inputs += module_path
    std_module_specs += name + '=' + module_path
endforeach

embed_target = custom_target(
    'embed-stdlib',
    input: std_module_inputs,
    output: generated_cpp_name,
    command: [python3, embed_std_modules_script, '@OUTPUT@', prelude_input]
    + std_module_specs,
    build_by_default: true,
)

builtins_decls_input = meson.current_source_dir() / 'lsp' / 'builtins.d.nari'
builtins_decls_cpp_name = 'embedded_builtins.cpp'
builtins_decls_cpp = generated_dir / builtins_decls_cpp_name

embed_builtins_target = custom_target(
    'embed-builtins',
    input: builtins_decls_input,
    output: builtins_decls_cpp_name,
    command: [
        python3,
        embed_script,
        builtins_decls_input,
        '@OUTPUT@',
        'nari_embedded_builtins',
    ],
    build_by_default: not disable_lsp,
)

sources = [
    'src/interpreter.cpp',
    'src/repl.cpp',
    'src/runtime.cpp',
    'src/builtins/core.cpp',
    'src/builtins/collections.cpp',
    'src/builtins/modules_io.cpp',
    'src/builtins/ffi.cpp',
    'src/builtins/hash.cpp',
    'src/builtins/archive.cpp',
    'src/builtins/platform_gc.cpp',
    'src/core_types.cpp',
    'src/gc.cpp',
    'src/nari_fs.cpp',
    'src/bytecode/bytecode.cpp',
    'src/parser.cpp',
    'src/parser/module_resolver.cpp',
    'src/parser/lexer.cpp',
    'src/debugger.cpp',
    'src/dap/dap_server.cpp',
]

if not target_mcu
    sources += [
        'src/bytecode/bytecode_compiler.cpp',
        embed_target,
    ]
endif

if not disable_ffi
    sources += ['src/ffi.cpp']
endif

if not disable_jit
    sources += [
        'src/jit/jit_helpers.cpp',
        'src/jit/jit_tls.cpp',
        'src/jit/trace_jit.cpp',
        'src/jit/trace_jit_asmjit.cpp',
        'src/jit/asmjit_jit.cpp',
        'src/jit/ir.cpp',
        'src/jit/ir_build.cpp',
        'src/jit/ir_opt.cpp',
        'src/jit/gdb_jit.cpp',
        'src/jit/perf_jitdump.cpp',
    ]
endif

if not is_emscripten
    inc_dirs = include_directories(
        conan_deploy / 'asmjit' / 'include',
        conan_deploy / 'mbedtls' / 'include',
        conan_deploy / 'zlib' / 'include',
        conan_deploy / 'libcurl' / 'include',
        conan_deploy / 'libffi' / 'include',
        conan_deploy / 'replxx' / 'include',
        conan_deploy / 'libarchive' / 'include',
    )
else
    inc_dirs = []
endif

all_deps = []

# Optional: override glibc malloc with mimalloc, increases performance slightly
# when available; -Dmimalloc=disabled to opt out.
if get_option('mimalloc') != 'disabled'
    _mimalloc_lib = cc.find_library('mimalloc', static: true, required: false)
    if not _mimalloc_lib.found()
        _mimalloc_lib = cc.find_library('mimalloc', required: get_option('mimalloc') == 'enabled')
    endif
    if _mimalloc_lib.found()
        all_deps += [_mimalloc_lib]
    endif
endif

if not disable_jit
    if system_deps
        # shared system asmjit: no ASMJIT_STATIC define
        asmjit_dep = dependency('asmjit', required: false)
        if not asmjit_dep.found()
            asmjit_dep = cc.find_library('asmjit', required: true)
        endif
    else
        _asmjit_lib = cc.find_library('asmjit', dirs: conan_asmjit_libdir, required: false)
        if not _asmjit_lib.found()
            _asmjit_lib = cc.find_library('asmjit', required: true)
        endif

        asmjit_dep = declare_dependency(
            dependencies: [_asmjit_lib],
            compile_args: ['-DASMJIT_STATIC=ON'],
        )
    endif
    all_deps += [asmjit_dep]
endif

if not is_emscripten and not disable_http and system_deps
    # System curl carries its own TLS/zlib. mbedtls stays a direct dependency
    _sys_http = [dependency('libcurl', required: true)]
    foreach mbedlib : ['mbedtls', 'mbedx509', 'mbedcrypto']
        _mbed_pc = dependency(mbedlib, required: false)
        if _mbed_pc.found()
            _sys_http += [_mbed_pc]
        else
            _sys_http += [cc.find_library(mbedlib, required: true)]
        endif
    endforeach
    http_dep = declare_dependency(dependencies: _sys_http)
    all_deps += [http_dep]
elif not is_emscripten and not disable_http
    curl_lib = cc.find_library('curl', dirs: conan_curl_libdir, required: true)
    mbedtls_lib = cc.find_library('mbedtls', dirs: conan_mbedtls_libdir, required: true)
    mbedx509_lib = cc.find_library('mbedx509', dirs: conan_mbedtls_libdir, required: true)
    mbedcrypto_lib = cc.find_library('mbedcrypto', dirs: conan_mbedtls_libdir, required: true)
    zlib_name = is_windows ? 'zlib' : 'z'
    zlib_lib = cc.find_library(zlib_name, dirs: conan_zlib_libdir, required: true)
    http_sys = [zlib_lib]
    http_compile_args = []
    if is_windows
        http_compile_args += ['-DCURL_STATICLIB']
        foreach syslib : [
            'ws2_32',
            'wldap32',
            'crypt32',
            'normaliz',
            'advapi32',
            'user32',
            'bcrypt',
            'iphlpapi',
        ]
            http_sys += [cc.find_library(syslib, required: true)]
        endforeach
    endif
    http_dep = declare_dependency(
        dependencies: [curl_lib, mbedtls_lib, mbedx509_lib, mbedcrypto_lib] + http_sys,
        compile_args: http_compile_args,
    )
    all_deps += [http_dep]
endif

if not is_emscripten and not disable_ffi
    if system_deps
        ffi_dep = dependency('libffi', required: false)
        if not ffi_dep.found()
            ffi_dep = cc.find_library('ffi', required: true)
        endif
    else
        ffi_dep = cc.find_library('ffi', dirs: conan_ffi_libdir, required: false)
        if not ffi_dep.found()
            ffi_dep = cc.find_library('ffi', required: true)
        endif
    endif
    all_deps += [ffi_dep]
endif

# libarchive (read + write, tar/gzip/zip).
if not is_emscripten and system_deps
    # shared system libarchive links its own zlib
    libarchive_dep = dependency('libarchive', required: false)
    if not libarchive_dep.found()
        libarchive_dep = cc.find_library('archive', required: true)
    endif
    all_deps += [libarchive_dep]
elif not is_emscripten
    libarchive_lib = cc.find_library('archive', dirs: conan_libarchive_libdir, required: true)
    archive_extra = []
    archive_compile_args = []
    if disable_http
        archive_zlib_name = is_windows ? 'zlib' : 'z'
        archive_extra += [
            cc.find_library(archive_zlib_name, dirs: conan_zlib_libdir, required: true),
        ]
    endif
    if is_windows
        archive_compile_args += ['-DLIBARCHIVE_STATIC']
        foreach syslib : ['xmllite', 'bcrypt']
            archive_extra += [cc.find_library(syslib, required: false)]
        endforeach
    endif
    libarchive_dep = declare_dependency(
        dependencies: [libarchive_lib] + archive_extra,
        compile_args: archive_compile_args,
    )
    all_deps += [libarchive_dep]
endif

if have_replxx
    all_deps += [_replxx_lib]
endif

if is_emscripten or is_windows
    platform_deps = []
elif is_darwin
    # macOS: dl is part of libSystem, libc++ is the default stdlib.
    platform_deps = []
else
    dl_dep = cc.find_library('dl', required: true)
    if cc.has_header_symbol('version', '_LIBCPP_VERSION')
        _cxx_lib = cc.find_library('c++', required: true)
        platform_deps = [dl_dep, _cxx_lib]
    else
        platform_deps = [dl_dep]
    endif
endif

all_deps += platform_deps

if is_emscripten
    tests_dir = meson.project_source_root() / 'tests'
    shell_file = meson.project_source_root() / 'shell.html'
    link_args += [
        '--preload-file', tests_dir + '@/tests/',
        '--shell-file', shell_file,
        '-o', 'nari.html',
    ]
endif

executable(
    'nari',
    sources,
    include_directories: [
        inc_dirs,
        include_directories('src', 'src/dap', 'src/bytecode', 'src/jit'),
    ],
    dependencies: all_deps,
    link_args: link_args,
    install: true,
)

if not is_emscripten and not target_mcu
    naric_sources = [
        'src/naric.cpp',
        'src/nari_fs.cpp',
        'src/parser.cpp',
        'src/parser/module_resolver.cpp',
        'src/parser/lexer.cpp',
        'src/core_types.cpp',
        'src/gc.cpp',
        'src/bytecode/bytecode_compiler.cpp',
        embed_target,
    ]

    naric_deps = []
    if not is_windows
        naric_deps += platform_deps
    endif

    executable(
        'naric',
        naric_sources,
        include_directories: [inc_dirs, include_directories('src', 'src/bytecode', 'src/jit')],
        dependencies: naric_deps,
        install: true,
    )

    if not disable_lsp
        lsp_sources = [
            'lsp/lsp_server.cpp',
            'src/nari_fs.cpp',
            'src/parser.cpp',
            'src/parser/module_resolver.cpp',
            'src/parser/lexer.cpp',
            'src/core_types.cpp',
            'src/gc.cpp',
            'src/bytecode/bytecode_compiler.cpp',
            embed_target,
            embed_builtins_target,
        ]

        lsp_deps = []
        if not is_windows
            lsp_deps += platform_deps
        endif

        lsp_inc_dirs = [include_directories('src', 'src/bytecode', 'src/jit')]
        if not is_emscripten
            lsp_inc_dirs += [inc_dirs]
        endif

        executable(
            'nari-lsp',
            lsp_sources,
            include_directories: lsp_inc_dirs,
            dependencies: lsp_deps,
            install: true,
        )
    endif
endif