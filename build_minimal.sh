#!/usr/bin/env fish

# Build Nari with FFI and HTTP disabled (minimal build)
# This is useful for embedded targets or reducing binary size

set -euo pipefail

echo "Building Nari with FFI and HTTP disabled..."

# Clean previous build
if test -d build/minimal
  echo "Cleaning previous minimal build..."
  rm -rf build/minimal
end

# Configure with meson
meson setup build/minimal \
  --buildtype=release \
  -Ddisable_ffi=true \
  -Ddisable_http=true \
  -Ddisable_jit=true \
  -Ddisable_repl=true \
  --cross-file=toolchain/glibc-clang.txt

# Build
meson compile -C build/minimal

echo ""
echo "Minimal build complete!"
echo "Binary: build/minimal/interpreter"
echo ""
echo "Disabled features:"
echo "  - FFI (Foreign Function Interface)"
echo "  - HTTP/HTTPS networking"
echo "  - JIT Compilation (AsmJIT)"
