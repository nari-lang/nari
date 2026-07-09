#!/usr/bin/env bash
#
# npkg search smoke test.
#
# Verifies the `npkg search` CLI subcommand end-to-end against a local
# registry. The test skips cleanly if no registry is reachable so it can
# stay in run_tests.sh even on machines that don't run the server.
#
# Coverage:
#   1. Missing query exits non-zero with a usage hint.
#   2. Unknown query reports "No packages found".
#   3. Matching query lists package name + pagination summary.
#   4. --per-page <small> + --page <large> still terminates cleanly.
#   5. Unreachable registry exits non-zero with a connect error.
#
# Quiet on success; exits 0 even when the registry is down (printed as [skip]).

set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
NPKG="$REPO_ROOT/npkg-frontend/npkg/main.nari"
INTERPRETER="$REPO_ROOT/build/release/nari"

if [ ! -f "$NPKG" ] || [ ! -x "$INTERPRETER" ]; then
  echo "[skip] npkg search smoke: interpreter or entry script missing"
  exit 0
fi

REGISTRY="${NPKG_REGISTRY:-http://localhost:8080}"

# Probe the registry: if it's down, skip cleanly. Use a 2s connect timeout so
# the suite doesn't stall on machines that don't run the server.
if ! curl -fsS --max-time 2 "$REGISTRY/api/packages?per_page=1" >/dev/null 2>&1; then
  echo "[skip] npkg search smoke: registry not reachable at $REGISTRY"
  exit 0
fi

SCRATCH="$(mktemp -d -t npkg-search-smoke-XXXXXX)"
trap 'rm -rf "$SCRATCH"' EXIT

FAIL=0
fail() { echo "[fail] $*" >&2; FAIL=1; }

run_search() {
  # Usage: run_search OUTFILE [search args...]
  local out="$1"; shift
  "$INTERPRETER" "$NPKG" search "$@" --registry "$REGISTRY" >"$out" 2>&1
  echo $? >"$out.exit"
}

# Pick a query that should match at least one package. We bias to "demo"
# because the dev fixtures usually publish under that prefix; if not, the
# test falls back to listing the first known package name.
PROBE="$(curl -fsS --max-time 2 "$REGISTRY/api/packages?per_page=1" 2>/dev/null || true)"
FIRST_NAME="$(printf '%s' "$PROBE" | sed -n 's/.*"name":"\([^"]*\)".*/\1/p' | head -1)"
if [ -z "$FIRST_NAME" ]; then
  echo "[skip] npkg search smoke: registry has no packages to query"
  exit 0
fi
# Use the first path segment of the package name as the query so scoped
# names like '@demo/lib-a' still hit. The server matches LIKE %q%, so any
# distinctive substring works.
QUERY="$(printf '%s' "$FIRST_NAME" | sed 's|@||; s|/.*||')"
if [ -z "$QUERY" ]; then QUERY="$FIRST_NAME"; fi

# --- 1. No query -> usage error ---
OUT1="$SCRATCH/no-query.out"
"$INTERPRETER" "$NPKG" search --registry "$REGISTRY" >"$OUT1" 2>&1
RC1=$?
if [ "$RC1" = "0" ]; then
  cat "$OUT1" >&2
  fail "no-query: expected non-zero exit"
fi
grep -qiE "usage|search <query>" "$OUT1" \
  || fail "no-query: expected usage hint in error (got: $(head -1 "$OUT1"))"

# --- 2. Unknown query -> 'No packages found' ---
OUT2="$SCRATCH/unknown.out"
run_search "$OUT2" "definitelydoesnotexist_$$"
if [ "$(cat "$OUT2.exit")" != "0" ]; then
  cat "$OUT2" >&2
  fail "unknown: expected exit 0 even with zero results"
fi
grep -q "No packages found" "$OUT2" \
  || fail "unknown: expected 'No packages found' message"

# --- 3. Matching query lists package name + summary ---
OUT3="$SCRATCH/match.out"
run_search "$OUT3" "$QUERY"
if [ "$(cat "$OUT3.exit")" != "0" ]; then
  cat "$OUT3" >&2
  fail "match: expected exit 0 for query '$QUERY'"
fi
grep -qE "^Found [0-9]+ result\(s\) for '" "$OUT3" \
  || fail "match: expected 'Found N result(s)' header"
grep -qE "^Page [0-9]+, showing [0-9]+ of [0-9]+ result\(s\)" "$OUT3" \
  || fail "match: expected pagination summary line"
grep -qF "$FIRST_NAME" "$OUT3" \
  || fail "match: expected package name '$FIRST_NAME' in output"

# --- 4. Tiny per-page + far page -> terminates cleanly ---
OUT4="$SCRATCH/farpage.out"
run_search "$OUT4" "$QUERY" --per-page 1 --page 99
if [ "$(cat "$OUT4.exit")" != "0" ]; then
  cat "$OUT4" >&2
  fail "farpage: expected exit 0 (empty page should not error)"
fi

# --- 5. Unreachable registry -> connect error ---
OUT5="$SCRATCH/badreg.out"
"$INTERPRETER" "$NPKG" search "$QUERY" --registry "http://127.0.0.1:1" >"$OUT5" 2>&1
RC5=$?
if [ "$RC5" = "0" ]; then
  cat "$OUT5" >&2
  fail "badreg: expected non-zero exit on connect failure"
fi

if [ "$FAIL" -ne 0 ]; then
  echo "npkg search smoke: FAILED" >&2
  exit 1
fi

echo "npkg search smoke: PASS"
exit 0
