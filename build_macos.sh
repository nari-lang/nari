#!/usr/bin/env bash
# Cross-compile Nari for macOS ARM64 (Apple Silicon) from Linux
#
# Prerequisites:
#  osxcross installed at /opt/osxcross with macOS SDK 14.0+

set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJ_ROOT/build/macos-arm64"
DEPS_DIR="$BUILD_DIR/deps"
OSXCROSS="/opt/osxcross"

ASMJIT_SRC_DIR="$PROJ_ROOT/build/arm64/deps/asmjit-src"
LIBFFI_SRC_DIR="/tmp/libffi-latest"

export PATH="$OSXCROSS/bin:$PATH"

CC=arm64-apple-darwin23-clang
CXX=arm64-apple-darwin23-clang++
AR=arm64-apple-darwin23-ar
RANLIB=arm64-apple-darwin23-ranlib

# Verify osxcross is available
if ! command -v "$CXX" &>/dev/null; then
  echo "Error: $CXX not found. Install osxcross first."
  echo "  git clone https://github.com/tpoechtrager/osxcross /tmp/osxcross"
  echo "  Download macOS SDK to /tmp/osxcross/tarballs/"
  echo "  cd /tmp/osxcross && UNATTENDED=1 TARGET_DIR=/opt/osxcross ./build.sh"
  exit 1
fi

do_clean() {
  echo "=== Cleaning macOS ARM64 build ==="
  rm -rf "$BUILD_DIR"
}

build_asmjit() {
  local asmjit_build="$DEPS_DIR/asmjit"
  if [ -f "$asmjit_build/libasmjit.a" ]; then
  echo "asmjit already built, skipping."
  return
  fi

  # Find asmjit source
  local src_dir=""
  if [ -d "$ASMJIT_SRC_DIR" ]; then
  src_dir="$ASMJIT_SRC_DIR"
  else
  echo "Cloning asmjit..."
  git clone --depth 1 https://github.com/asmjit/asmjit.git "$DEPS_DIR/asmjit-src"
  src_dir="$DEPS_DIR/asmjit-src"
  fi

  echo "=== Building asmjit for macOS ARM64 ==="
  mkdir -p "$asmjit_build"

  cat > "$asmjit_build/toolchain.cmake" <<CMEOF
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_CXX_COMPILER $CXX)
set(CMAKE_AR $AR CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB $RANLIB CACHE FILEPATH "Ranlib")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
CMEOF

  cd "$asmjit_build"
  cmake "$src_dir" \
  -DCMAKE_TOOLCHAIN_FILE="$asmjit_build/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DASMJIT_STATIC=ON \
  -DASMJIT_TEST=OFF \
  -DASMJIT_NO_FOREIGN=ON
  make -j"$(nproc)"
  echo "asmjit built."
}

build_libffi() {
  local ffi_build="$DEPS_DIR/libffi"
  if [ -f "$ffi_build/install/lib/libffi.a" ]; then
  echo "libffi already built, skipping."
  return
  fi

  # Find libffi source
  local src_dir=""
  if [ -d "$LIBFFI_SRC_DIR" ] && [ -f "$LIBFFI_SRC_DIR/configure" ]; then
  src_dir="$LIBFFI_SRC_DIR"
  else
  echo "Cloning libffi..."
  git clone --depth 1 https://github.com/libffi/libffi.git "$DEPS_DIR/libffi-src"
  cd "$DEPS_DIR/libffi-src" && ./autogen.sh
  src_dir="$DEPS_DIR/libffi-src"
  fi

  echo "=== Building libffi for macOS ARM64 ==="
  mkdir -p "$ffi_build"
  cd "$ffi_build"

  CC=aarch64-apple-darwin23-clang \
  CXX=aarch64-apple-darwin23-clang++ \
  AR=aarch64-apple-darwin23-ar \
  RANLIB=aarch64-apple-darwin23-ranlib \
  "$src_dir/configure" \
  --host=aarch64-apple-darwin23 \
  --disable-shared --enable-static \
  --prefix="$ffi_build/install"
  make -j"$(nproc)"
  make install
  echo "libffi built."
}

do_build() {
  mkdir -p "$BUILD_DIR" "$DEPS_DIR"

  build_asmjit
  build_libffi

  ASMJIT_INC="$DEPS_DIR/asmjit"
  # asmjit headers may be in the source tree
  if [ -d "$ASMJIT_SRC_DIR/asmjit" ]; then
  ASMJIT_INC_SRC="$ASMJIT_SRC_DIR"
  else
  ASMJIT_INC_SRC="$DEPS_DIR/asmjit-src"
  fi
  ASMJIT_LIB="$DEPS_DIR/asmjit"
  FFI_INC="$DEPS_DIR/libffi/install/include"
  FFI_LIB="$DEPS_DIR/libffi/install/lib"

  echo "=== Configuring Nari for macOS ARM64 ==="

  # Generate Meson cross file
  cat > "$BUILD_DIR/cross.txt" <<XEOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = 'arm64-apple-darwin23-strip'
ranlib = '$RANLIB'

[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[built-in options]
cpp_args = ['-I$ASMJIT_INC_SRC', '-I$FFI_INC', '-stdlib=libc++', '-mmacosx-version-min=13.3']
cpp_link_args = ['-L$ASMJIT_LIB', '-L$FFI_LIB', '-stdlib=libc++', '-mmacosx-version-min=13.3']
XEOF

  cd "$PROJ_ROOT"
  if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  meson setup "$BUILD_DIR" \
  --cross-file "$BUILD_DIR/cross.txt" \
  --buildtype release \
  -Ddisable_jit=false \
  -Ddisable_ffi=false \
  -Ddisable_http=true \
  -Ddisable_repl=true
  else
  meson setup --reconfigure "$BUILD_DIR" \
  --cross-file "$BUILD_DIR/cross.txt" \
  --buildtype release \
  -Ddisable_jit=false \
  -Ddisable_ffi=false \
  -Ddisable_http=true \
  -Ddisable_repl=true
  fi

  echo "=== Building Nari for macOS ARM64 ==="
  ninja -C "$BUILD_DIR"
  echo ""
  echo "=== Build complete ==="
  file "$BUILD_DIR/nari"
}

case "${1:-build}" in
  build) do_build ;;
  clean) do_clean ;;
  *)
  echo "Usage: $0 [build|clean]"
  exit 1
  ;;
esac
