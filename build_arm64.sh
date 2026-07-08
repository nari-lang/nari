#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="build/arm64"
DEPS_DIR="build/arm64/deps"
CROSS_FILE="toolchain/aarch64-linux.txt"
QEMU="qemu-aarch64"
SYSROOT="/usr/aarch64-linux-gnu"
LIBFFI_VERSION="3.4.6"

# qemu needs the ARM64 dynamic linker + libs
QEMU_FLAGS="-L $SYSROOT"

# Allow extra qemu flags via environment
QEMU_FLAGS="${QEMU_EXTRA_FLAGS:-} $QEMU_FLAGS"

action="${1:-build}"

case "$action" in
  clean)
  echo "Removing $BUILD_DIR..."
  rm -rf "$BUILD_DIR"
  exit 0
  ;;
esac

# Cross-compile libffi for aarch64 if not already done
LIBFFI_PREFIX="$DEPS_DIR/libffi"
if [ ! -f "$LIBFFI_PREFIX/lib/libffi.a" ]; then
  echo "=== Building libffi for AArch64 ==="
  mkdir -p "$DEPS_DIR"
  LIBFFI_SRC="$DEPS_DIR/libffi-src"

  if [ ! -d "$LIBFFI_SRC" ]; then
  echo "Downloading libffi ${LIBFFI_VERSION}..."
  curl -sL "https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/libffi-${LIBFFI_VERSION}.tar.gz" \
  | tar xz -C "$DEPS_DIR"
  mv "$DEPS_DIR/libffi-${LIBFFI_VERSION}" "$LIBFFI_SRC"
  fi

  pushd "$LIBFFI_SRC" > /dev/null
  CC="clang --target=aarch64-linux-gnu --sysroot=$SYSROOT" \
  AR=llvm-ar \
  RANLIB=llvm-ranlib \
  ./configure \
  --prefix="$(cd .. && pwd)/libffi" \
  --host=aarch64-linux-gnu \
  --enable-static \
  --disable-shared \
  --with-pic \
  --quiet
  make -j"$(nproc)" --quiet
  make install --quiet
  popd > /dev/null
  echo "libffi built for AArch64: $LIBFFI_PREFIX"
fi

# Cross-compile asmjit for aarch64 if not already done
ASMJIT_PREFIX="$DEPS_DIR/asmjit"
if [ ! -f "$ASMJIT_PREFIX/lib/libasmjit.a" ]; then
  echo "=== Building asmjit for AArch64 ==="
  mkdir -p "$DEPS_DIR"
  ASMJIT_SRC="$DEPS_DIR/asmjit-src"

  if [ ! -d "$ASMJIT_SRC" ]; then
  echo "Cloning asmjit..."
  git clone --depth 1 https://github.com/asmjit/asmjit.git "$ASMJIT_SRC"
  fi

  ASMJIT_BUILD="$DEPS_DIR/asmjit-build"
  mkdir -p "$ASMJIT_BUILD"

  cmake -S "$ASMJIT_SRC" -B "$ASMJIT_BUILD" \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="--target=aarch64-linux-gnu --sysroot=$SYSROOT" \
  -DCMAKE_CXX_FLAGS="--target=aarch64-linux-gnu --sysroot=$SYSROOT -I/usr/aarch64-linux-gnu/include/c++/15.1.0 -I/usr/aarch64-linux-gnu/include/c++/15.1.0/aarch64-linux-gnu" \
  -DCMAKE_AR=$(which llvm-ar) \
  -DCMAKE_RANLIB=$(which llvm-ranlib) \
  -DCMAKE_INSTALL_PREFIX="$ASMJIT_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DASMJIT_STATIC=ON \
  -DASMJIT_TEST=OFF \
  -DASMJIT_NO_NATVIS=ON \
  > /dev/null

  cmake --build "$ASMJIT_BUILD" -j"$(nproc)" > /dev/null
  cmake --install "$ASMJIT_BUILD" > /dev/null
  echo "asmjit built for AArch64: $ASMJIT_PREFIX"
fi

# Generate a cross file that includes the libffi and asmjit paths
ARM64_CROSS="$BUILD_DIR/cross.txt"
mkdir -p "$BUILD_DIR"
LIBFFI_ABS="$(cd "$LIBFFI_PREFIX" && pwd)"
ASMJIT_ABS="$(cd "$ASMJIT_PREFIX" && pwd)"
cat > "$ARM64_CROSS" <<EOF
[binaries]
c = 'clang'
cpp = 'clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['--target=aarch64-linux-gnu', '--sysroot=$SYSROOT', '-fdiagnostics-absolute-paths', '-Wno-unused-command-line-argument', '-I$LIBFFI_ABS/include', '-I$ASMJIT_ABS/include']
cpp_args = ['--target=aarch64-linux-gnu', '--sysroot=$SYSROOT', '-fdiagnostics-absolute-paths', '-Wno-unused-variable', '-Wno-unused-lambda-capture', '-Wno-unused-command-line-argument', '-Wno-unused-function', '-I/usr/aarch64-linux-gnu/include/c++/15.1.0', '-I/usr/aarch64-linux-gnu/include/c++/15.1.0/aarch64-linux-gnu', '-I$LIBFFI_ABS/include', '-I$ASMJIT_ABS/include']
c_link_args = ['--target=aarch64-linux-gnu', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$LIBFFI_ABS/lib', '-L$ASMJIT_ABS/lib']
cpp_link_args = ['--target=aarch64-linux-gnu', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$SYSROOT/lib', '-lstdc++', '-L$LIBFFI_ABS/lib', '-L$ASMJIT_ABS/lib']
EOF

# Configure if needed
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  echo "=== Configuring ARM64 build ==="
  meson setup "$BUILD_DIR" \
  --cross-file "$ARM64_CROSS" \
  --buildtype=release \
  -Ddisable_jit=false \
  -Ddisable_ffi=false \
  -Ddisable_http=true \
  -Ddisable_repl=true
fi

echo "=== Building for ARM64 ==="
ninja -C "$BUILD_DIR"

case "$action" in
  build)
  echo ""
  echo "Build complete: $BUILD_DIR/interpreter"
  echo "Run with: $QEMU $QEMU_FLAGS $BUILD_DIR/interpreter <file.nari>"
  ;;
  run)
  shift
  file="${1:?Usage: build_arm64.sh run FILE.nari}"
  shift || true
  echo ""
  echo "=== Running under QEMU (ARM64) ==="
  $QEMU $QEMU_FLAGS "$BUILD_DIR/interpreter" "$file" "$@"
  ;;
  test)
  echo ""
  echo "=== Running tests under QEMU (ARM64) ==="
  pass=0
  fail=0
  skip=0
  for t in tests/expect_pass/*.nari; do
  name=$(basename "$t")
  if output=$($QEMU $QEMU_FLAGS "$BUILD_DIR/interpreter" "$t" 2>&1); then
  echo "  PASS  $name"
  pass=$((pass + 1))
  else
  echo "  FAIL  $name"
  echo "  $output" | head -3
  fail=$((fail + 1))
  fi
  done
  for t in tests/expect_fail/*.nari; do
  name=$(basename "$t")
  if $QEMU $QEMU_FLAGS "$BUILD_DIR/interpreter" "$t" >/dev/null 2>&1; then
  echo "  FAIL  $name (expected failure but passed)"
  fail=$((fail + 1))
  else
  echo "  PASS  $name (expected failure)"
  pass=$((pass + 1))
  fi
  done
  echo ""
  echo "Results: $pass passed, $fail failed"
  [ "$fail" -eq 0 ]
  ;;
  *)
  echo "Unknown action: $action"
  echo "Usage: build_arm64.sh [build|run FILE|test|clean]"
  exit 1
  ;;
esac
