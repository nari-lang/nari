set -euo pipefail

BUILD_TYPE=debug
RECONFIGURE=false
WIPE=false
MUSL=false
EMSCRIPTEN=false
SANITIZE=false
SYSTEM_DEPS=false
CROSS_FILE="glibc-clang.txt"
MESON_OPTS=""

EXTRA_ARGS=""

# Windows builds run natively via GitHub Actions CI. See .github/workflows/build-windows.yml.

for arg in "$@"; do
  if [ "$arg" = "--release" ]; then
  echo "Building as release..."
  BUILD_TYPE=release
  # b_lto: whole-program optimization. b_ndebug: compile out asserts (Meson's
  # 'release' buildtype does NOT do this automatically).
  EXTRA_ARGS="$EXTRA_ARGS -Db_lto=true -Db_ndebug=true"
  elif [ "$arg" = "--reconfigure" ]; then
  echo "Forcing reconfigure..."
  RECONFIGURE=true
  elif [ "$arg" = "--musl" ]; then
  echo "WARNING: Building with musl is very experimental, and FFI is known to not work properly. Here be dragons!"
  echo "Building with musl..."
  MUSL=true
  CROSS_FILE="musl-clang.txt"
  elif [ "$arg" = "--emscripten" ]; then
  echo "Building for WebAssembly with Emscripten..."
  EMSCRIPTEN=true
  CROSS_FILE="emscripten.txt"
  MESON_OPTS="$MESON_OPTS -Ddisable_ffi=true -Ddisable_http=true -Ddisable_jit=true"
  # emscripten builds always use release for performance
  BUILD_TYPE=release
  EXTRA_ARGS="$EXTRA_ARGS -Db_lto=false"
  elif [ "$arg" = "--sanitize" ]; then
  echo "Building with AddressSanitizer + UndefinedBehaviorSanitizer..."
  SANITIZE=true
  BUILD_TYPE=debug
  # -Db_lundef=false lets the sanitizer runtime's undefined symbols
  # resolve at link time (required with LTO off).
  # Disable mimalloc: it overrides malloc, which clashes with ASan's malloc
  # interceptors (both want to own the allocator).
  EXTRA_ARGS="$EXTRA_ARGS -Db_sanitize=address,undefined -Db_lundef=false -Dmimalloc=disabled"
  elif [ "$arg" = "--system-deps" ]; then
  # Distro-packaging mode: no Conan; link system packages via pkg-config / default linker paths, and build with the system toolchain

  echo "Building against system dependencies (no Conan)..."
  SYSTEM_DEPS=true
  MESON_OPTS="$MESON_OPTS -Dsystem_deps=true"
  elif [ "$arg" = "--wipe" ]; then
  echo "Wiping build directory..."
  WIPE=true
  RECONFIGURE=true
  else
  case "$arg" in
  -D*)
  # forward meson -D options (e.g. -Ddisable_jit=true) untouched
  MESON_OPTS="$MESON_OPTS $arg"
  ;;
  esac
  fi
done

if [ "$SANITIZE" = true ]; then
  BUILD_DIR="sanitize"
elif [ "$SYSTEM_DEPS" = true ]; then
  BUILD_DIR="sysdeps-$BUILD_TYPE"
elif [ "$MUSL" = true ]; then
  BUILD_DIR="musl-$BUILD_TYPE"
elif [ "$EMSCRIPTEN" = true ]; then
  BUILD_DIR="emscripten-$BUILD_TYPE"
else
  BUILD_DIR=$BUILD_TYPE
fi

if [ ! -d build/$BUILD_DIR ]; then
  RECONFIGURE=true
fi

# run conan, but only if not using emscripten, since emscripten provides everything we need
if [ "$EMSCRIPTEN" != true ] && [ "$SYSTEM_DEPS" != true ]; then
  echo "Installing dependencies via conan..."
  mkdir -p build/conan
  # Build deps against libc++ to match toolchain/glibc-clang.txt. The two MUST
  # agree: a libc++ replxx linked into a libstdc++ binary is a mixed-runtime build
  # that links against both .so's and fails intermittently. -s compiler.libcxx
  # overrides only that one setting on top of the detected default profile.
  CONAN_LIBCXX_ARG=""
  if [ "$CROSS_FILE" = "glibc-clang.txt" ]; then
  CONAN_LIBCXX_ARG="-s compiler.libcxx=libc++"
  fi
  conan install . $CONAN_LIBCXX_ARG --output-folder=build/conan --deployer=direct_deploy --deployer-folder=build/conan --build=missing
fi

if [ "$RECONFIGURE" = true ]; then
  MESON_SETUP_FLAGS=""
  if [ "$WIPE" = true ]; then
  MESON_SETUP_FLAGS="--wipe"
  fi
  if [ "$SYSTEM_DEPS" = true ]; then
  # no cross file (system toolchain + system STL) and no conan native file
  meson setup $MESON_SETUP_FLAGS build/$BUILD_DIR/ --buildtype=$BUILD_TYPE $EXTRA_ARGS $MESON_OPTS
  else
  meson setup $MESON_SETUP_FLAGS build/$BUILD_DIR/ --cross-file toolchain/$CROSS_FILE --native-file build/conan/conan_meson_native.ini --buildtype=$BUILD_TYPE $EXTRA_ARGS $MESON_OPTS
  fi
fi

meson compile -C build/$BUILD_DIR/

# Post-process the Wasm binary with wasm-opt if available
if [ "$EMSCRIPTEN" = true ]; then
  WASM_FILE="build/$BUILD_DIR/nari.wasm"
  if command -v wasm-opt >/dev/null 2>&1; then
  echo "Running wasm-opt on $WASM_FILE..."
  wasm-opt --generate-global-effects --monomorphize --pass-arg=monomorphize-min-benefit@75 -O4 --enable-bulk-memory --enable-sign-ext --gufa --closed-world -O4 --strip-toolchain-annotations --flatten --rereloop -O4 -O4 -o "$WASM_FILE" "$WASM_FILE"
  echo "wasm-opt done."
  else
  echo "wasm-opt not found; skipping (install binaryen to enable)"
  fi
fi
