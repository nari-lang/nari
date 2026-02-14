#!/bin/bash
set -e

# Detect target (default to linux)
TARGET=${TARGET:-linux}
XWIN_CACHE="$HOME/.xwin-cache/splat/"

cd thirdparty/openssl/

if [ "$TARGET" = "windows" ]; then
    echo "Building OpenSSL for Windows with clang..."
    
    # Set up environment for cross-compilation to Windows
    export CC="clang-cl --target=x86_64-pc-windows-msvc"
    export CXX="clang-cl --target=x86_64-pc-windows-msvc"
    export AR="llvm-ar"
    export RANLIB="llvm-ranlib"
    export RC="llvm-rc"
    
    # Add Windows SDK include paths
    export CFLAGS="-imsvc ${XWIN_CACHE}/crt/include -imsvc ${XWIN_CACHE}/sdk/include/ucrt -imsvc ${XWIN_CACHE}/sdk/include/um -imsvc ${XWIN_CACHE}/sdk/include/shared"
    export CXXFLAGS="$CFLAGS"
    
    # Configure for Windows using mingw64 target (works for cross-compilation)
    ./Configure mingw64 \
      --prefix=$PWD/build-windows \
      --openssldir=/etc/ssl \
      no-shared \
      no-dso \
      no-engine \
      no-comp \
      no-tests \
      no-apps \
      no-docs \
      no-asm \
      no-blake2 \
      no-camellia \
      no-cast \
      no-chacha \
      no-cmac \
      no-des \
      no-dh \
      no-dsa \
      no-ec2m \
      no-idea \
      no-md2 \
      no-md4 \
      no-mdc2 \
      no-poly1305 \
      no-rc2 \
      no-rc4 \
      no-rc5 \
      no-rmd160 \
      no-scrypt \
      no-seed \
      no-siphash \
      no-sm2 \
      no-sm3 \
      no-sm4 \
      no-srp \
      no-whirlpool \
      no-aria \
      no-bf \
      no-gost \
      no-ocsp \
      no-cms \
      no-ts \
      no-srtp \
      no-ssl3 \
      no-tls1 \
      no-tls1_1 \
      no-dtls \
      no-psk \
      -Os
else
    echo "Building OpenSSL for Linux..."
    
    ./config \
      --prefix=$PWD/build-linux \
      --openssldir=/etc/ssl \
      no-shared \
      no-dso \
      no-engine \
      no-comp \
      no-tests \
      no-apps \
      no-docs \
      no-asm \
      no-blake2 \
      no-camellia \
      no-cast \
      no-chacha \
      no-cmac \
      no-des \
      no-dh \
      no-dsa \
      no-ec2m \
      no-idea \
      no-md2 \
      no-md4 \
      no-mdc2 \
      no-poly1305 \
      no-rc2 \
      no-rc4 \
      no-rc5 \
      no-rmd160 \
      no-scrypt \
      no-seed \
      no-siphash \
      no-sm2 \
      no-sm3 \
      no-sm4 \
      no-srp \
      no-whirlpool \
      no-aria \
      no-bf \
      no-gost \
      no-ocsp \
      no-cms \
      no-ts \
      no-srtp \
      no-ssl3 \
      no-tls1 \
      no-tls1_1 \
      no-dtls \
      no-psk \
      -Os
fi

make -j$(nproc)
make install_sw

echo "OpenSSL built successfully for $TARGET!"
