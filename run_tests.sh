#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPE="release"
BUILD_ARG="--release"
FLAGS=()

for arg in "$@"; do
    case "$arg" in
        --debug)
            BUILD_TYPE="debug"
            BUILD_ARG=""
        ;;
        --release)
            BUILD_TYPE="release"
            BUILD_ARG="--release"
        ;;
        --sanitize)
            BUILD_TYPE="sanitize"
            BUILD_ARG="--sanitize"
        ;;
        *)
            echo "Unknown test option: $arg" >&2
            echo "Usage: ./run_tests.sh [--release|--debug|--sanitize]" >&2
            exit 2
        ;;
    esac
done

INTERP="build/${BUILD_TYPE}/nari"
NARIC="build/${BUILD_TYPE}/naric"
LSP="build/${BUILD_TYPE}/nari-lsp"

# Sanitizer runtime options.
# This suite targets undefined-behaviour / use-after-free / out-of-bounds /
# detect_leaks and detect_container_overflow are off because they produce false positives.
if [ "$BUILD_TYPE" = "sanitize" ]; then
    export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:halt_on_error=1:detect_leaks=0:detect_container_overflow=0:strict_string_checks=1}"
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
    echo "Sanitizer mode: ASAN_OPTIONS=$ASAN_OPTIONS ; UBSAN_OPTIONS=$UBSAN_OPTIONS"
fi

# SKIP_BUILD=1 lets a caller (e.g. CI) that already compiled the binary run the
# suite against the existing build/${BUILD_TYPE} tree without reconfiguring.
if [ "${SKIP_BUILD:-0}" = "1" ]; then
    echo "SKIP_BUILD=1 set; using existing build at build/${BUILD_TYPE}"
    elif [ -n "$BUILD_ARG" ]; then
    ./build.sh "$BUILD_ARG"
else
    ./build.sh
fi

if [ ! -x "$INTERP" ]; then
    echo "ERROR: interpreter not found or not executable: $INTERP" >&2
    exit 2
fi

python3 tools/run_language_tests.py --interpreter "$INTERP"

echo "Running FFI bindgen union tests..."
if ! NARI="$INTERP" bash tests/ffi_bindgen_union.sh; then
    echo "[fail] tests/ffi_bindgen_union.sh" >&2
    exit 1
fi

# Additional tests that need shell or Python
EXTRA_SUITES=(
    "tests/file_io_errors.sh"
    "tests/trace_jit_execution.sh"
    "tests/naric_robustness.sh"
    "tests/archive_extract_security.py"
    "tests/archive_io.py"
    "tests/package_store_resolution.py"
    "tests/error_propagation.py"
    "tests/interrupt_shutdown.sh"
)

for suite in "${EXTRA_SUITES[@]}"; do
    if [ -x "$suite" ] || [ -f "$suite" ]; then
        echo "Running $suite..."
        case "$suite" in
            *.py) suite_command=(python3 "$suite" --interpreter "$INTERP") ;;
            *.sh) suite_command=(bash "$suite") ;;
            *) echo "Unsupported test suite: $suite" >&2; exit 2 ;;
        esac
        if ! INTERP="$INTERP" NARIC="$NARIC" LSP="$LSP" "${suite_command[@]}"; then
            echo "[fail] $suite" >&2
            exit 1
        fi
    fi
done

echo "All tests completed!"
