#!/usr/bin/env fish

# Build Nari with FFI and HTTP disabled (minimal build)
# This is useful for embedded targets or reducing binary size

echo "Building Nari with FFI and HTTP disabled..."

# Clean previous build
if test -d build/minimal
    echo "Cleaning previous minimal build..."
    rm -rf build/minimal
end

# Configure with meson
meson setup build/minimal \
    --buildtype=release \
    -Denable_ffi=false \
    -Denable_http=false

# Build
meson compile -C build/minimal

echo ""
echo "Minimal build complete!"
echo "Binary: build/minimal/interpreter"
echo ""
echo "Disabled features:"
echo "  - FFI (Foreign Function Interface)"
echo "  - HTTP/HTTPS networking"
echo "  - TCP server/client"
echo ""
echo "To run: ./build/minimal/interpreter your_script.nari"
