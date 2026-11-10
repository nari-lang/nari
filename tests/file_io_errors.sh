#!/usr/bin/env bash
set -euo pipefail

INTERP="${INTERP:-build/release/nari}"
NARIC="${NARIC:-build/release/naric}"
TEST_TMP=$(mktemp -d)
writer_pid=""
cleanup() {
    if [[ -n "$writer_pid" ]]; then
        kill "$writer_pid" 2>/dev/null || true
        wait "$writer_pid" 2>/dev/null || true
    fi
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT

TIMEOUT=()
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT=(timeout 10)
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT=(gtimeout 10)
fi

expect_status() {
    local expected="$1"
    shift
    local status=0
    ${TIMEOUT[@]+"${TIMEOUT[@]}"} "$@" >"$TEST_TMP/stdout" 2>"$TEST_TMP/stderr" || status=$?
    if ((status != expected)); then
        echo "[fail] expected status $expected, got $status: $*" >&2
        cat "$TEST_TMP/stdout" "$TEST_TMP/stderr" >&2
        exit 1
    fi
}

: > "$TEST_TMP/empty.nari"
expect_status 0 "$INTERP" fmt --check "$TEST_TMP/empty.nari"
expect_status 0 "$INTERP" fmt -w "$TEST_TMP/empty.nari"
test ! -s "$TEST_TMP/empty.nari"

for path in "$TEST_TMP" "$TEST_TMP/missing.nari"; do
    expect_status 2 "$INTERP" "$path"
    expect_status 2 "$NARIC" "$path"
    expect_status 2 "$INTERP" fmt "$path"
done
expect_status 2 "$INTERP" fmt --stdin < "$TEST_TMP"

printf 'print("file reader checks passed");\n' > "$TEST_TMP/source.nari"
expect_status 0 "$NARIC" --no-stdlib -o "$TEST_TMP/source.naric" "$TEST_TMP/source.nari"
expect_status 0 "$INTERP" "$TEST_TMP/source.naric"

# A malformed imported module must stop execution before any script code runs.
printf 'BAD' > "$TEST_TMP/bad.naric"
printf 'import "./bad.naric"; print("should not run");\n' > "$TEST_TMP/import.nari"
expect_status 7 "$INTERP" "$TEST_TMP/import.nari"
test ! -s "$TEST_TMP/stdout"

# File readers must work without a seekable input or a reliable file size.
mkfifo "$TEST_TMP/pipe.nari"
cat "$TEST_TMP/source.nari" > "$TEST_TMP/pipe.nari" &
writer_pid=$!
expect_status 0 "$INTERP" "$TEST_TMP/pipe.nari"
wait "$writer_pid"
writer_pid=""

if [[ -c /dev/full ]]; then
    # This bytecode is small enough that failure may occur only at flush/close.
    expect_status 5 "$NARIC" --no-stdlib -o /dev/full "$TEST_TMP/source.nari"
    local_status=0
    ${TIMEOUT[@]+"${TIMEOUT[@]}"} "$INTERP" fmt --stdin < "$TEST_TMP/source.nari" > /dev/full 2> "$TEST_TMP/stderr" || local_status=$?
    if ((local_status != 2)); then
        echo "[fail] formatter ignored stdout write failure (status $local_status)" >&2
        exit 1
    fi
fi

echo "file I/O CLI checks passed"
