#!/usr/bin/env bash
set -euo pipefail

INTERP="${INTERP:-build/release/nari}"

check_trace() {
    local script="$1"
    local required_functions="$2"
    local output
    if ! output="$(NARI_TRACE_STATS=1 "$INTERP" "$script" 2>&1)"; then
        printf '%s\n' "$output" >&2
        echo "trace test failed: $script" >&2
        return 1
    fi
    local traced_functions
    traced_functions="$(printf '%s\n' "$output" | awk '/^\[trace-stats\]/ { for (i = 1; i <= NF; i++) if ($i ~ /^func=/) seen[$i] = 1 } END { for (f in seen) count++; print count + 0 }')"
    if (( traced_functions < required_functions )); then
        printf '%s\n' "$output" >&2
        echo "expected $required_functions traced functions in $script; saw $traced_functions" >&2
        return 1
    fi
    echo "trace executed in $script ($traced_functions traced function(s))"
}

check_trace tests/expect_pass/test_trace_jit_const_mod.nari 3
check_trace tests/expect_pass/test_jit_float_eq_ne.nari 1
check_trace tests/expect_pass/test_trace_jit_overflow.nari 4
check_trace tests/expect_pass/test_trace_jit_function_name.nari 1
