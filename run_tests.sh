#!/usr/bin/env bash
set -u pipefail

INTERP="build/minimal/interpreter"

./build.sh

echo "Running success tests..."
SUCCESS_TESTS=($(find tests/expect_pass -name "*.nari"))

PASSED_TESTS=0
FAILED_TESTS=0
FAILED_LIST=()

for t in "${SUCCESS_TESTS[@]}"; do
  if [[ -f "$t" ]]; then
    echo "[expected_ok] $t"
    if "$INTERP" "$t"; then
      ((PASSED_TESTS++))
    else
      echo "[fail] $t" >&2
      FAILED_LIST+=("$t")
      ((FAILED_TESTS++))
    fi
  else
    echo "[skip] $t (missing)"
  fi
done

echo "Running expected-failure tests..."
FAIL_TESTS=($(find tests/expect_fail -name "*.nari"))

for t in "${FAIL_TESTS[@]}"; do
  if [[ -f "$t" ]]; then
    echo "[expected_fail] $t"
    if "$INTERP" "$t"; then
      echo "Unexpected success for $t" >&2
      FAILED_LIST+=("$t")
      ((FAILED_TESTS++))
    else
      ((PASSED_TESTS++))
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
echo "All tests completed!"
