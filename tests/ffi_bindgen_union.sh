#!/usr/bin/env bash
set -euo pipefail

NARI="${NARI:-build/release/nari}"
OUT="${TMPDIR:-/tmp}/nari-ffi-bindgen-union-$$.nari"
trap 'rm -f "$OUT"' EXIT

check_output() {
    local backend="$1"
    shift
    "$NARI" tools/ffi_bindgen.nari --input tests/ffi_bindgen_union.h --output "$OUT" --backend "$backend" "$@"
    grep -q '^union SDL_Event {' "$OUT"
    grep -q '^    padding: u8\[56\];' "$OUT"
    grep -q '^type EventHolder {' "$OUT"
    grep -q '^    event: SDL_Event;' "$OUT"
}

check_output legacy
if command -v clang >/dev/null 2>&1; then
    check_output clang-json --cc clang
fi
