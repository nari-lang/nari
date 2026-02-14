#!/bin/bash
set -e

LIBFFI_VERSION="3.5.2"
LIBFFI_DIR="thirdparty/libffi"
TARGET=${TARGET:-linux}
XWIN_CACHE="/home/wearr/.xwin-cache/splat"

# Create thirdparty directory if it doesn't exist
mkdir -p thirdparty
cd thirdparty

# Download libffi if not already present
if [ ! -d "libffi-${LIBFFI_VERSION}" ]; then
    echo "Downloading libffi ${LIBFFI_VERSION}..."
    wget https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/libffi-${LIBFFI_VERSION}.tar.gz
    tar xzf libffi-${LIBFFI_VERSION}.tar.gz
    rm libffi-${LIBFFI_VERSION}.tar.gz
fi

# Build static library
cd libffi-${LIBFFI_VERSION}

if [ "$TARGET" = "windows" ]; then
    BUILD_DIR="build-windows"
    echo "Building static libffi for Windows..."
    
    if [ ! -f "${BUILD_DIR}/lib/libffi.a" ]; then
        # For libffi, we use MinGW target (autotools compatible)
        # The resulting .a file is still usable with MSVC-targeted code
        export CC="clang --target=x86_64-w64-mingw32"
        export CXX="clang++ --target=x86_64-w64-mingw32"
        export AR="llvm-ar"
        export RANLIB="llvm-ranlib"
        # MinGW target doesn't need xwin SDK paths - it uses MinGW headers
        unset CFLAGS
        unset CXXFLAGS
        unset LDFLAGS
        
        # Configure for Windows cross-compilation
        ./configure \
            --prefix=$(pwd)/${BUILD_DIR} \
            --host=x86_64-w64-mingw32 \
            --enable-static \
            --disable-shared \
            --with-pic
        
        # Build
        make -j$(nproc)
        
        # Install to build directory
        make install
        
        echo "libffi built successfully for Windows!"
    else
        echo "libffi already built for Windows"
    fi
    
    echo "libffi static library is at: thirdparty/libffi-${LIBFFI_VERSION}/${BUILD_DIR}/lib/libffi.a"
    echo "libffi headers are at: thirdparty/libffi-${LIBFFI_VERSION}/${BUILD_DIR}/include"
else
    BUILD_DIR="build"
    echo "Building static libffi for Linux..."
    
    if [ ! -f "${BUILD_DIR}/lib/libffi.a" ]; then
        # Configure for static library
        ./configure \
            --prefix=$(pwd)/${BUILD_DIR} \
            --enable-static \
            --disable-shared \
            --with-pic
        
        # Build
        make -j$(nproc)
        
        # Install to build directory
        make install
        
        echo "libffi built successfully for Linux!"
    else
        echo "libffi already built for Linux"
    fi
    
    echo "libffi static library is at: thirdparty/libffi-${LIBFFI_VERSION}/${BUILD_DIR}/lib/libffi.a"
    echo "libffi headers are at: thirdparty/libffi-${LIBFFI_VERSION}/${BUILD_DIR}/include"
fi

cd ../..
