#!/usr/bin/env bash
# `nari fmt` tests: golden fixtures, corpus idempotency, and semantics preservation.
# Standalone (like tests/naric_robustness.sh); not wired into run_tests.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_TYPE="${BUILD_TYPE:-release}"
NARI="$ROOT/build/$BUILD_TYPE/nari"
FIX="$ROOT/tests/fmt"

if [ ! -x "$NARI" ]; then
    echo "ERROR: interpreter not found: $NARI (build it first)" >&2
    exit 2
fi

fail=0

echo "== golden fixtures =="
for input in "$FIX"/*.input.nari; do
    expected="${input%.input.nari}.expected.nari"
    name="$(basename "$input" .input.nari)"
    if ! out="$("$NARI" fmt "$input" 2>&1)"; then
        echo "[fail] $name (formatter error: $out)"
        fail=1
        continue
    fi
    # $() strips trailing newlines on both sides, so only content is compared
    if [ "$out" != "$(cat "$expected")" ]; then
        echo "[fail] $name"
        diff <(printf '%s\n' "$out") "$expected" | head -20 || true
        fail=1
    else
        echo "[pass] $name"
    fi
done

echo "== corpus idempotency =="
tmp1="$(mktemp)"
tmp2="$(mktemp)"
count=0
for f in "$ROOT"/tests/expect_pass/*.nari "$ROOT"/tests/expect_fail/*.nari \
         "$ROOT"/examples/*.nari "$ROOT"/src/stdlib/std/*.nari; do
    [ -f "$f" ] || continue
    count=$((count + 1))
    if ! "$NARI" fmt "$f" > "$tmp1" 2>/dev/null; then
        echo "[fail] formatter error: $f"
        fail=1
        continue
    fi
    "$NARI" fmt "$tmp1" > "$tmp2"
    if ! cmp -s "$tmp1" "$tmp2"; then
        echo "[fail] not idempotent: $f"
        diff "$tmp1" "$tmp2" | head -10 || true
        fail=1
    fi
done
rm -f "$tmp1" "$tmp2"
echo "idempotency checked on $count files"

echo "== semantics preservation =="
tmpd="$(mktemp -d)"
trap 'rm -rf "$tmpd"' EXIT
cp -r "$ROOT/tests" "$tmpd/tests"
while IFS= read -r t; do
    if ! "$NARI" fmt -w "$t"; then
        echo "[fail] fmt -w: ${t#"$tmpd"/}"
        fail=1
    fi
done < <(find "$tmpd/tests" -name '*.nari')

run_pass=0
run_fail=0
NETWORK_TESTS_RE="test_spawn_methods"
while IFS= read -r t; do
    case "$(basename "$t")" in
        *npkg*) continue ;;
    esac
    if [ "${SKIP_NETWORK_TESTS:-0}" = "1" ] && [[ "$(basename "$t")" =~ $NETWORK_TESTS_RE ]]; then
        continue
    fi
    if timeout 30 "$NARI" "$t" > /dev/null 2>&1; then
        run_pass=$((run_pass + 1))
    else
        echo "[fail] formatted test broke: ${t#"$tmpd"/}"
        run_fail=$((run_fail + 1))
        fail=1
    fi
done < <(find "$tmpd/tests/expect_pass" -name '*.nari' | sort)
echo "formatted expect_pass: $run_pass passed, $run_fail failed"

still_failing=0
now_passing=0
for t in "$tmpd"/tests/expect_fail/*.nari; do
    [ -f "$t" ] || continue
    if timeout 30 "$NARI" "$t" > /dev/null 2>&1; then
        echo "[fail] expect_fail now passes after formatting: ${t#"$tmpd"/}"
        now_passing=$((now_passing + 1))
        fail=1
    else
        still_failing=$((still_failing + 1))
    fi
done
echo "formatted expect_fail: $still_failing still failing, $now_passing unexpectedly passing"

if [ "$fail" -eq 0 ]; then
    echo "nari fmt: all tests passed"
else
    echo "nari fmt: FAILURES above" >&2
fi
exit "$fail"
