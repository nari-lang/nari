#!/usr/bin/env bash
set -euo pipefail

INTERP="${INTERP:-build/release/nari}"
MAX_WAIT_TICKS="${MAX_WAIT_TICKS:-100}"

run_interrupt_test() {
    local name="$1"
    shift

    "$@" >/dev/null 2>&1 &
    local pid=$!
    sleep 0.2
    kill -INT "$pid"

    local ticks=0
    while kill -0 "$pid" 2>/dev/null; do
        if ((ticks >= MAX_WAIT_TICKS)); then
            kill -KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            echo "[fail] $name ignored SIGINT" >&2
            return 1
        fi
        sleep 0.02
        ((ticks += 1))
    done

    local status=0
    wait "$pid" || status=$?
    if ((status != 0)); then
        echo "[fail] $name exited with status $status" >&2
        return 1
    fi
    echo "[pass] $name"
}

run_interrupt_test "bytecode VM" env NARI_DISABLE_JIT=1 "$INTERP" tests/interrupt/tight_loop.nari
run_interrupt_test "trace JIT" "$INTERP" tests/interrupt/trace_jit_loop.nari
run_interrupt_test "method JIT" "$INTERP" tests/interrupt/method_jit_loop.nari
