#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPE="release"
BUILD_ARG="--release"
FLAGS=()

for arg in "$@"; do
    case "$arg" in
        --tree-walk)
            FLAGS+=("--tree-walk")
        ;;
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
            echo "Usage: ./run_tests.sh [--release|--debug|--sanitize] [--tree-walk]" >&2
            exit 2
        ;;
    esac
done

INTERP="build/${BUILD_TYPE}/nari"
NARIC="build/${BUILD_TYPE}/naric"

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

# npkg tests depend on the workspace/npkg tree, which is not committed, so they
# are excluded here (they fail in CI with unresolved imports otherwise).
echo "Running success tests with $INTERP..."
mapfile -t SUCCESS_TESTS < <(find tests/expect_pass -name "*.nari" -not -name "*npkg*" | sort)

# SKIP_NETWORK_TESTS=1 drops tests that reach external hosts (which are flaky in CI).
# currently just test_spawn_methods.nari
NETWORK_TESTS=("tests/expect_pass/test_spawn_methods.nari")
if [ "${SKIP_NETWORK_TESTS:-0}" = "1" ]; then
    echo "SKIP_NETWORK_TESTS=1 set; excluding network-dependent tests"
    FILTERED=()
    for t in "${SUCCESS_TESTS[@]}"; do
        skip=0
        for n in "${NETWORK_TESTS[@]}"; do
            if [ "$t" = "$n" ]; then skip=1; break; fi
        done
        if [ "$skip" -eq 0 ]; then FILTERED+=("$t"); fi
    done
    SUCCESS_TESTS=("${FILTERED[@]}")
fi

PASSED_TESTS=0
FAILED_TESTS=0
FAILED_LIST=()

# Per-test timeout. Most tests finish in well under a second, 
# but a few (e.g. tests/expect_pass/test_spawn_methods.nari) can run for longer, since it uses real endpoints
TEST_TIMEOUT="${TEST_TIMEOUT:-30}"

for t in "${SUCCESS_TESTS[@]}"; do
    if [[ -f "$t" ]]; then
        echo "[expected_ok] $t"
        if timeout "$TEST_TIMEOUT" "$INTERP" "${FLAGS[@]}" "$t"; then
            ((PASSED_TESTS += 1))
        else
            exit_code=$?
            if [ "$exit_code" -eq 124 ]; then
                echo "[timeout] $t (killed after ${TEST_TIMEOUT}s)" >&2
            else
                echo "[fail] $t" >&2
            fi
            FAILED_LIST+=("$t")
            ((FAILED_TESTS += 1))
        fi
    else
        echo "[skip] $t (missing)"
    fi
done

echo "Running expected-failure tests..."
mapfile -t FAIL_TESTS < <(find tests/expect_fail -name "*.nari" | sort)

for t in "${FAIL_TESTS[@]}"; do
    if [[ -f "$t" ]]; then
        echo "[expected_fail] $t"
        if timeout "$TEST_TIMEOUT" "$INTERP" "${FLAGS[@]}" "$t"; then
            echo "Unexpected success for $t" >&2
            FAILED_LIST+=("$t")
            ((FAILED_TESTS += 1))
        else
            ((PASSED_TESTS += 1))
        fi
    else
        echo "[skip] $t (missing)"
    fi
done

echo "Passed tests: $PASSED_TESTS / $(( ${#SUCCESS_TESTS[@]} + ${#FAIL_TESTS[@]} ))"
if (( FAILED_TESTS > 0 )); then
    echo "Failed tests: ${FAILED_TESTS}" >&2
    for t in "${FAILED_LIST[@]}"; do
        echo " - $t" >&2
    done
    exit 1
fi

# Shell-level suites that can't be expressed as .nari files
EXTRA_SUITES=(
    "tests/naric_robustness.sh"
    # "tests/npkg_workspace_smoke.sh"
    # "tests/npkg_publish_polish.sh"
    # "tests/npkg_search_smoke.sh"
    # "tests/npkg_owner_smoke.sh"
)

# for suite in "${EXTRA_SUITES[@]}"; do
#   if [ -x "$suite" ] || [ -f "$suite" ]; then
#   echo "Running $suite..."
#   if ! INTERP="$INTERP" NARIC="$NARIC" bash "$suite"; then
#   echo "[fail] $suite" >&2
#   exit 1
#   fi
#   fi
# done

echo "All tests completed!"
