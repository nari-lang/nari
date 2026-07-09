# Building for Windows

Windows builds run natively on Windows. CI is handled automatically by the
GitHub Actions workflow at `.github/workflows/build-windows.yml`.

## Prerequisites

1. Visual Studio 2022 (MSVC), or LLVM/clang-cl if preferred
2. Python + pip: `pip install meson ninja conan`

## Install Dependencies

All dependencies (OpenSSL, libffi, AsmJIT) are managed by [Conan](https://conan.io/).

```bat
conan profile detect --force
conan install . --output-folder=build\conan --deployer=direct_deploy --deployer-folder=build\conan --build=missing
```

## Build

The easiest way to build is with `build.ps1`, which automatically activates
the VS x64 environment (equivalent to running from an *x64 Native Tools
Command Prompt*) before invoking conan and meson:

```powershell
.\build.ps1  # debug
.\build.ps1 -Release  # release
.\build.ps1 -ClangCl  # clang-cl instead of MSVC
```

### Manual build

If you prefer to run commands manually you **must** first activate the x64 VS
environment, otherwise clang-cl and MSVC pick up x86 library paths:

```bat
:: From an x64 Native Tools Command Prompt for VS 2022, or run vcvarsall first:
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64

conan profile detect --force
conan install . --output-folder=build\conan --deployer=direct_deploy --deployer-folder=build\conan --build=missing

:: Debug
meson setup build\debug --native-file build\conan\conan_meson_native.ini --buildtype=debug
meson compile -C build\debug

:: Release
meson setup build\release --native-file build\conan\conan_meson_native.ini --buildtype=release -Db_lto=true
meson compile -C build\release
```

Output: `build\release\nari.exe` and `build\release\naric.exe`.

## Using clang-cl instead of MSVC

Pass `-ClangCl` to `build.ps1`, or manually add the native file:

```bat
meson setup build\release ^
  --native-file build\conan\conan_meson_native.ini ^
  --native-file toolchain\windows-clang-cl.txt ^
  --buildtype=release
```

## Building From Linux With Wine

If you are on Linux, the repo now has a Wine-driven path that does **not**
reuse your default Wine prefix or require a full Visual Studio install inside
Wine. It creates a fresh prefix under `build/wine-prefix`, unpacks a portable
Windows Python runtime into that prefix, downloads a portable MSVC + Windows
SDK toolchain with `install_msvc.py`, and then runs the normal Conan + Meson
build under Wine.

Recommended usage:

```bash
./build_wine.sh --release
./build_wine.sh --win7 --release
```

Useful options:

```bash
./build_wine.sh --recreate-prefix  # start from a clean Wine prefix
./build_wine.sh --clang-cl  # use clang-cl instead of cl.exe
./build_wine.sh --win7  # enable the Windows 7 compatibility shim
./build_wine.sh --win7 --static-crt # test a /MT-based standalone runtime build
./build_wine.sh --win7 --vs 2022  # force the VS 2022 manifest line
./build_wine.sh --win7 --toolset v143
./build_wine.sh --msvc-version 14.44 --sdk-version 22621
```

Host prerequisites:

1. `wine`, `wineboot`, `wineserver`, `winepath`
2. `curl`
3. Linux `python3`

Notes:

1. This flow keeps Wine state inside `build/`, so it does not touch your
  existing default prefix.
2. The first run is slow because it downloads Windows Python, MSVC, the
  Windows SDK, and Conan packages.
3. The Windows-side build entrypoint is `build_wine.cmd` if you want to invoke
  it manually with `wine cmd /c`.

## CI

The GitHub Actions workflow (`.github/workflows/build-windows.yml`) runs on
`windows-latest` and uploads `nari.exe` + `naric.exe` as build
artifacts on every push to `main`.
