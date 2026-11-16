#!/usr/bin/env bash
set -euo pipefail

INTERP="${INTERP:-build/release/nari}"
MAX_WAIT_TICKS="${MAX_WAIT_TICKS:-100}"
TEST_TMP=$(mktemp -d)
FILE_PARTNER_PID=""
stop_file_partner() {
    if [[ -n "$FILE_PARTNER_PID" ]]; then
        kill "$FILE_PARTNER_PID" 2>/dev/null || true
        wait "$FILE_PARTNER_PID" 2>/dev/null || true
        FILE_PARTNER_PID=""
    fi
}
cleanup() { stop_file_partner; rm -rf "$TEST_TMP"; }
trap cleanup EXIT

run_shutdown_test() {
    local name="$1"
    shift

    local log="$TEST_TMP/output.log"
    "$@" >"$log" 2>&1 &
    local pid=$!
    if [ "${SEND_INTERRUPT:-1}" = "1" ]; then
        sleep 0.2
        kill -INT "$pid"
    fi

    local ticks=0
    while kill -0 "$pid" 2>/dev/null; do
        if ((ticks >= MAX_WAIT_TICKS)); then
            kill -KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            echo "[fail] $name did not shut down" >&2
            cat "$log" >&2
            return 1
        fi
        sleep 0.02
        ((ticks += 1))
    done

    local status=0
    wait "$pid" || status=$?
    if ((status != ${EXPECTED_STATUS:-0})); then
        echo "[fail] $name exited with status $status" >&2
        cat "$log" >&2
        return 1
    fi
    if [[ -n "${EXPECTED_OUTPUT:-}" && "$(<"$log")" != *"$EXPECTED_OUTPUT"* ]]; then
        echo "[fail] $name did not print '$EXPECTED_OUTPUT'" >&2
        cat "$log" >&2
        return 1
    fi
    echo "[pass] $name"
}

run_shutdown_test "bytecode VM" env NARI_DISABLE_JIT=1 "$INTERP" tests/interrupt/tight_loop.nari
run_shutdown_test "trace JIT" "$INTERP" tests/interrupt/trace_jit_loop.nari
run_shutdown_test "method JIT" "$INTERP" tests/interrupt/method_jit_loop.nari
run_shutdown_test "pending timeout" "$INTERP" tests/interrupt/pending_timeout.nari

export NARI_TEST_FIFO="$TEST_TMP/pending.pipe"
mkfifo "$NARI_TEST_FIFO"
for operation in read write append queued; do
    NARI_TEST_FILE_OP="$operation" run_shutdown_test "pending file $operation" "$INTERP" tests/interrupt/pending_file.nari
done
test ! -e "$NARI_TEST_FIFO.queued"
SEND_INTERRUPT=0 NARI_TEST_FILE_OP=normal EXPECTED_OUTPUT="pending files scheduled" \
    run_shutdown_test "normal exit with pending files" "$INTERP" tests/interrupt/pending_file.nari
test ! -e "$NARI_TEST_FIFO.queued"

SEND_INTERRUPT=0 NARI_TEST_FILE_OP=exit EXPECTED_OUTPUT="exit with pending files" \
    run_shutdown_test "process.exit with pending files" "$INTERP" tests/interrupt/pending_file.nari
test ! -e "$NARI_TEST_FIFO.queued"

# Connect a peer that keeps its end open without making progress.
(exec 3> "$NARI_TEST_FIFO"; exec sleep 10) &
FILE_PARTNER_PID=$!
NARI_TEST_FILE_OP=read run_shutdown_test "connected FIFO read" "$INTERP" tests/interrupt/pending_file.nari
stop_file_partner
(exec 3< "$NARI_TEST_FIFO"; exec sleep 10) &
FILE_PARTNER_PID=$!
NARI_TEST_FILE_OP=write run_shutdown_test "full FIFO write" "$INTERP" tests/interrupt/pending_file.nari
stop_file_partner
(exec 3< "$NARI_TEST_FIFO"; sleep 0.5) &
FILE_PARTNER_PID=$!
SEND_INTERRUPT=0 NARI_TEST_FILE_OP=broken EXPECTED_OUTPUT="closed FIFO write returned Err" \
    run_shutdown_test "closed FIFO peer" "$INTERP" tests/interrupt/pending_file.nari
stop_file_partner

if [ "${SKIP_NETWORK_TESTS:-0}" != "1" ]; then
    EXPECTED_OUTPUT="idle server scheduled" run_shutdown_test "idle TCP server" "$INTERP" tests/interrupt/idle_server.nari
    run_shutdown_test "pending TCP read" "$INTERP" tests/interrupt/pending_tcp_read.nari
    run_shutdown_test "pending TCP accept await" "$INTERP" tests/interrupt/pending_accept_await.nari
    run_shutdown_test "pending HTTP response" "$INTERP" tests/interrupt/pending_http.nari
    SEND_INTERRUPT=0 EXPECTED_OUTPUT="exit with pending HTTP" \
        run_shutdown_test "exit with pending HTTP" "$INTERP" tests/interrupt/pending_http_exit.nari
    SEND_INTERRUPT=0 EXPECTED_STATUS=8 EXPECTED_OUTPUT="server handler and GC checks passed" \
        run_shutdown_test "server callbacks and panic cleanup" "$INTERP" tests/interrupt/server_callbacks_gc.nari
fi
