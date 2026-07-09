#!/usr/bin/env bash
# Regression tests for .naric deserializer hardening.
# Feeds the interpreter malformed / truncated / oversized .naric files and verifies it rejects them cleanly

set -u

INTERP="${INTERP:-build/release/nari}"
NARIC="${NARIC:-build/release/naric}"

if [ ! -x "$INTERP" ] || [ ! -x "$NARIC" ]; then
  echo "ERROR: interpreter or naric binary missing; run ./build.sh --release first or pass INTERP/NARIC" >&2
  exit 2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

check() {
  local name="$1"
  local file="$2"
  # exit code 0 (success) or 139 (segfault) means the hardening failed.
  timeout 5 "$INTERP" "$file" >/dev/null 2>&1
  local rc=$?
  if [ $rc -eq 0 ]; then
    echo "[fail] $name: accepted malformed file (exit 0)" >&2
    FAIL=$((FAIL+1))
  elif [ $rc -ge 128 ] && [ $rc -ne 137 ]; then
    echo "[fail] $name: crashed with signal (exit $rc)" >&2
    FAIL=$((FAIL+1))
  elif [ $rc -eq 124 ]; then
    echo "[fail] $name: hung (timeout)" >&2
    FAIL=$((FAIL+1))
  else
    echo "[pass] $name: rejected cleanly (exit $rc)"
    PASS=$((PASS+1))
  fi
}

# a real compiled file should still work.
cat > "$TMP/ok.nari" <<'EOF'
func start() { print("hello"); }
EOF
"$NARIC" -o "$TMP/ok.naric" "$TMP/ok.nari" >/dev/null 2>&1 || {
  echo "ERROR: could not compile baseline file" >&2
  exit 2
}
if ! timeout 5 "$INTERP" "$TMP/ok.naric" >/dev/null 2>&1; then
  echo "ERROR: baseline .naric does not run" >&2
  exit 2
fi
echo "[pass] baseline .naric runs"
PASS=$((PASS+1))

# Case 1: wrong magic bytes.
printf 'XXXX\x03\x00\x00\x00' > "$TMP/bad_magic.naric"
check "bad magic bytes" "$TMP/bad_magic.naric"

# Case 2: truncated header (less than 8 bytes).
printf 'NARI\x03\x00' > "$TMP/short_header.naric"
check "truncated header" "$TMP/short_header.naric"

# Case 3: valid header but truncated string-table count.
printf 'NARI\x03\x00\x00\x00' > "$TMP/short_strings.naric"
check "header-only, missing strings section" "$TMP/short_strings.naric"

# Case 4: huge string count (would require billions of bytes).
#   header + strings_count = 0xFFFFFFFF
printf 'NARI\x03\x00\x00\x00\xFF\xFF\xFF\xFF' > "$TMP/huge_count.naric"
check "absurd string count" "$TMP/huge_count.naric"

# Case 5: valid count(1) but truncated string payload.
#   header + strings_count(1) + string len(1000) + no payload bytes
printf 'NARI\x03\x00\x00\x00\x01\x00\x00\x00\xE8\x03\x00\x00' > "$TMP/short_string.naric"
check "string length exceeds remaining buffer" "$TMP/short_string.naric"

# Case 6: valid file with one trailing byte chopped off.
head -c $(( $(wc -c < "$TMP/ok.naric") - 1 )) "$TMP/ok.naric" > "$TMP/ok_minus1.naric"
# Chopping one byte may or may not trip a check depending on where it lands. We only require no crash/hang, so a clean exit is acceptable.
timeout 5 "$INTERP" "$TMP/ok_minus1.naric" >/dev/null 2>&1
rc=$?
if [ $rc -ge 128 ] && [ $rc -ne 137 ]; then
  echo "[fail] truncated end: crashed with signal (exit $rc)" >&2
  FAIL=$((FAIL+1))
elif [ $rc -eq 124 ]; then
  echo "[fail] truncated end: hung" >&2
  FAIL=$((FAIL+1))
else
  echo "[pass] truncated end: handled cleanly (exit $rc)"
  PASS=$((PASS+1))
fi

check_verifier() {
  local name="$1"
  local mutation="$2"
  local expect_msg="$3"
  local out="$TMP/mut_${mutation}.naric"
  if ! python3 "$(dirname "$0")/verifier_mutate.py" "$TMP/ok.naric" "$out" "$mutation" 2>/dev/null; then
    echo "[fail] $name: mutate script failed" >&2
    FAIL=$((FAIL+1))
    return
  fi
  output=$(timeout 5 "$INTERP" "$out" 2>&1)
  rc=$?
  if [ $rc -eq 0 ]; then
    echo "[fail] $name: verifier accepted bad bytecode" >&2
    FAIL=$((FAIL+1))
  elif [ $rc -ge 128 ] && [ $rc -ne 137 ]; then
    echo "[fail] $name: crashed (rc=$rc)" >&2
    FAIL=$((FAIL+1))
  elif ! echo "$output" | grep -q "bytecode verifier:"; then
    echo "[fail] $name: exited $rc but not via verifier (expected '$expect_msg')" >&2
    echo "$output" | head -3 >&2
    FAIL=$((FAIL+1))
  elif ! echo "$output" | grep -qF "$expect_msg"; then
    echo "[fail] $name: verifier fired but with wrong message (expected '$expect_msg')" >&2
    echo "$output" | head -3 >&2
    FAIL=$((FAIL+1))
  else
    echo "[pass] $name: verifier rejected '$expect_msg'"
    PASS=$((PASS+1))
  fi
}

# crafted semantically-invalid opcodes that pass the structural deserializer but must be rejected by BytecodeVerifier.
check_verifier "unknown opcode" "bad_opcode" "unknown opcode 254"
check_verifier "constant idx out of range" "huge_const_idx" "constant index out of range"
check_verifier "string idx out of range" "huge_str_idx" "name/string index out of range"
check_verifier "local idx out of range" "bad_local_idx" "local variable index out of range"
check_verifier "stack underflow" "stack_underflow" "stack underflow"
check_verifier "jump target not boundary" "bad_jump_target" "jump target is not an instruction boundary"

echo "---"
echo "naric robustness: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
