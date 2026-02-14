BUILD_TYPE=debug
RECONFIGURE=false
MUSL=false
WINDOWS=false
EMSCRIPTEN=false
CROSS_FILE="glibc-clang.txt"

for arg in "$@"; do
    if [ "$arg" = "--release" ]; then
        echo "Building as release..."
        BUILD_TYPE=release
    elif [ "$arg" = "--reconfigure" ]; then
        echo "Forcing reconfigure..."
        RECONFIGURE=true
    elif [ "$arg" = "--musl" ]; then
        echo "WARNING: Building with musl is very experimental, and FFI is known to not work properly. Here be dragons!"
        echo "Building with musl..."
        MUSL=true
        CROSS_FILE="musl-clang.txt"
    elif [ "$arg" = "--windows" ]; then
        echo "Building for Windows with clang-cl..."
        WINDOWS=true
        CROSS_FILE="windows-clang-cl.txt"
    elif [ "$arg" = "--emscripten" ]; then
        echo "Building for WebAssembly with Emscripten..."
        EMSCRIPTEN=true
        CROSS_FILE="emscripten.txt"
    fi
done

# Determine build directory based on target platform
if [ "$WINDOWS" = true ]; then
    BUILD_DIR="windows-$BUILD_TYPE"
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

# Set environment variables for all build types
if [ "$WINDOWS" = true ]; then
    export INCLUDE="$HOME/.xwin-cache/splat/crt/include;$HOME/.xwin-cache/splat/sdk/include/ucrt;$HOME/.xwin-cache/splat/sdk/include/um;$HOME/.xwin-cache/splat/sdk/include/shared"
    export LIB="$HOME/.xwin-cache/splat/crt/lib/x86_64;$HOME/.xwin-cache/splat/sdk/lib/um/x86_64;$HOME/.xwin-cache/splat/sdk/lib/ucrt/x86_64"
    export PKG_CONFIG_PATH=$PWD/thirdparty/openssl/build-windows/lib/pkgconfig:$PKG_CONFIG_PATH
elif [ "$MUSL" = true ]; then
    export PKG_CONFIG_PATH=$PWD/thirdparty/openssl/build/lib/pkgconfig:$PKG_CONFIG_PATH
elif [ "$EMSCRIPTEN" != true ]; then
    export PKG_CONFIG_PATH=$PWD/thirdparty/openssl/build/lib/pkgconfig:$PKG_CONFIG_PATH
fi

if [ "$RECONFIGURE" = true ]; then
    meson setup build/$BUILD_DIR/ --cross-file toolchain/$CROSS_FILE --buildtype=$BUILD_TYPE
fi

meson compile -C build/$BUILD_DIR/
