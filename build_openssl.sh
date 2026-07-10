#!/usr/bin/env bash
set -euo pipefail

# Detect target (default to linux)
TARGET=${TARGET:-linux}
XWIN_CACHE="$HOME/.xwin-cache/splat/"

cd thirdparty/openssl/

if [ "$TARGET" = "windows" ]; then
  # OpenSSL for Windows is provided by Conan. see conanfile.txt.
  echo "OpenSSL for Windows is managed by Conan."
  echo "Run: conan install . --output-folder=build/conan --build=missing"
  exit 0
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
