# Building for Windows on Linux

This project supports cross-compilation to Windows using clang-cl and xwin.

## Prerequisites

1. Install LLVM/Clang with clang-cl support
2. Install xwin and set up the Windows SDK cache at `$HOME/.xwin-cache/` (default for xwin)

## Building Dependencies

Build OpenSSL and libffi for Windows:

```bash
# Build OpenSSL for Windows
TARGET=windows ./build_deps.sh

# Build libffi for Windows  
TARGET=windows ./build_libffi.sh
```

These scripts will create `build-windows` directories for the Windows builds:
- `thirdparty/openssl/build-windows/`
- `thirdparty/libffi-3.5.2/build-windows/`

## Building the Project

Use the build script with the `--windows` flag:

```bash
# Debug build
./build.sh --windows --reconfigure

# Release build
./build.sh --windows --release --reconfigure
```

Or manually with Meson:

```bash
meson setup build/windows --cross-file toolchain/windows-clang-cl.txt --buildtype=release
meson compile -C build/windows
```

## Output

The resulting `interpreter.exe` will be in:
- Debug: `build/windows-debug/interpreter.exe`
- Release: `build/windows-release/interpreter.exe`

## Testing

You can test the Windows binary on Linux using Wine:

```bash
wine build/windows-release/interpreter.exe test.nari
```
