#!/usr/bin/env bash
#
# npkg owner smoke test.
#
# Verifies the `npkg owner ls` CLI subcommand end-to-end against a local
# registry. Like the search smoke, this skips cleanly if no registry is
# reachable so the suite stays green on dev machines without a server.
#
# Coverage (read-only, no auth):
#   1. Missing package name -> non-zero exit + usage hint.
#   2. Unknown subcommand   -> non-zero exit, error mentions valid subcommands.
#   3. Unknown package name -> non-zero exit + 'package not found'.
#   4. Known package        -> exit 0, prints "Owners of <pkg>:" + at least
#                              one indented owner line.
#   5. Unreachable registry -> non-zero exit with a connect error.
#   6. owner add usage      -> missing args / bad username caught client-side.
#   7. owner rm usage       -> same.
#
# Coverage (auth, only when NPKG_TEST_TOKEN + NPKG_TEST_TOKEN_B + NPKG_TEST_USER_B
# + NPKG_TEST_PKG are set):
#   8.  add co-owner        -> exit 0, ls confirms membership.
#   9.  add idempotent      -> exit 0, message contains "already".
#   10. co-owner can rm     -> token B can remove user B itself.
#   11. last-owner refused  -> rm of the sole remaining owner returns 409.
#
# Quiet on success; exits 0 even when the registry is down (printed as [skip]).

set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
NPKG="$REPO_ROOT/npkg-frontend/npkg/main.nari"
INTERPRETER="$REPO_ROOT/build/release/nari"

if [ ! -f "$NPKG" ] || [ ! -x "$INTERPRETER" ]; then
  echo "[skip] npkg owner smoke: interpreter or entry script missing"
  exit 0
fi

REGISTRY="${NPKG_REGISTRY:-http://localhost:8080}"

# Probe the registry: if it's down, skip cleanly.
if ! curl -fsS --max-time 2 "$REGISTRY/api/packages?per_page=1" >/dev/null 2>&1; then
  echo "[skip] npkg owner smoke: registry not reachable at $REGISTRY"
  exit 0
fi

# Pick the first published package. If the registry is empty we can't run the
# happy-path check, but unknown-name and connect-failure cases still apply.
PROBE="$(curl -fsS --max-time 2 "$REGISTRY/api/packages?per_page=1" 2>/dev/null || true)"
FIRST_NAME="$(printf '%s' "$PROBE" | sed -n 's/.*"name":"\([^"]*\)".*/\1/p' | head -1)"

SCRATCH="$(mktemp -d -t npkg-owner-smoke-XXXXXX)"
trap 'rm -rf "$SCRATCH"' EXIT

FAIL=0
fail() { echo "[fail] $*" >&2; FAIL=1; }

# --- 1. Missing package name -> usage error ---
OUT1="$SCRATCH/no-name.out"
"$INTERPRETER" "$NPKG" owner ls --registry "$REGISTRY" >"$OUT1" 2>&1
RC1=$?
if [ "$RC1" = "0" ]; then
  cat "$OUT1" >&2
  fail "no-name: expected non-zero exit"
fi
grep -qiE "usage|owner ls <name>" "$OUT1" \
  || fail "no-name: expected usage hint (got: $(head -1 "$OUT1"))"

# --- 2. Unknown subcommand -> error mentions valid subcommands ---
OUT2="$SCRATCH/bad-sub.out"
"$INTERPRETER" "$NPKG" owner frobnicate --registry "$REGISTRY" >"$OUT2" 2>&1
RC2=$?
if [ "$RC2" = "0" ]; then
  cat "$OUT2" >&2
  fail "bad-sub: expected non-zero exit"
fi
grep -qiE "unknown owner subcommand|expected: ls" "$OUT2" \
  || fail "bad-sub: expected hint about valid subcommands (got: $(head -1 "$OUT2"))"

# --- 3. Unknown package -> not found ---
OUT3="$SCRATCH/unknown.out"
"$INTERPRETER" "$NPKG" owner ls "definitelydoesnotexist-$$" --registry "$REGISTRY" >"$OUT3" 2>&1
RC3=$?
if [ "$RC3" = "0" ]; then
  cat "$OUT3" >&2
  fail "unknown: expected non-zero exit for missing package"
fi
grep -qiE "not found|owner ls failed" "$OUT3" \
  || fail "unknown: expected 'not found' / 'owner ls failed' (got: $(head -1 "$OUT3"))"

# --- 4. Known package -> lists at least one owner ---
if [ -n "$FIRST_NAME" ]; then
  OUT4="$SCRATCH/match.out"
  "$INTERPRETER" "$NPKG" owner ls "$FIRST_NAME" --registry "$REGISTRY" >"$OUT4" 2>&1
  RC4=$?
  if [ "$RC4" != "0" ]; then
    cat "$OUT4" >&2
    fail "match: expected exit 0 for '$FIRST_NAME'"
  fi
  grep -qF "Owners of $FIRST_NAME:" "$OUT4" \
    || fail "match: expected 'Owners of $FIRST_NAME:' header"
  # At least one indented owner line (two leading spaces).
  grep -qE "^  [A-Za-z0-9._-]+" "$OUT4" \
    || fail "match: expected at least one owner line (got: $(sed -n '1,5p' "$OUT4"))"
else
  echo "[note] npkg owner smoke: registry empty, skipping happy-path check"
fi

# --- 5. Unreachable registry -> connect error ---
OUT5="$SCRATCH/badreg.out"
"$INTERPRETER" "$NPKG" owner ls "anything" --registry "http://127.0.0.1:1" >"$OUT5" 2>&1
RC5=$?
if [ "$RC5" = "0" ]; then
  cat "$OUT5" >&2
  fail "badreg: expected non-zero exit on connect failure"
fi

# --- 6. owner add: missing args + bad username ---
OUT6A="$SCRATCH/add-noargs.out"
"$INTERPRETER" "$NPKG" owner add --registry "$REGISTRY" --token dummy >"$OUT6A" 2>&1
RC6A=$?
if [ "$RC6A" = "0" ]; then fail "add-noargs: expected non-zero exit"; fi
grep -qiE "usage|owner add <name> <user>" "$OUT6A" \
  || fail "add-noargs: expected usage hint (got: $(head -1 "$OUT6A"))"

OUT6B="$SCRATCH/add-baduser.out"
"$INTERPRETER" "$NPKG" owner add somepkg "bad user!" --registry "$REGISTRY" --token dummy >"$OUT6B" 2>&1
RC6B=$?
if [ "$RC6B" = "0" ]; then fail "add-baduser: expected non-zero exit"; fi
grep -qi "invalid username" "$OUT6B" \
  || fail "add-baduser: expected 'invalid username' (got: $(head -1 "$OUT6B"))"

# --- 7. owner rm: missing args + bad username ---
OUT7A="$SCRATCH/rm-noargs.out"
"$INTERPRETER" "$NPKG" owner rm --registry "$REGISTRY" --token dummy >"$OUT7A" 2>&1
RC7A=$?
if [ "$RC7A" = "0" ]; then fail "rm-noargs: expected non-zero exit"; fi
grep -qiE "usage|owner rm <name> <user>" "$OUT7A" \
  || fail "rm-noargs: expected usage hint (got: $(head -1 "$OUT7A"))"

OUT7B="$SCRATCH/rm-baduser.out"
"$INTERPRETER" "$NPKG" owner rm somepkg "x/y" --registry "$REGISTRY" --token dummy >"$OUT7B" 2>&1
RC7B=$?
if [ "$RC7B" = "0" ]; then fail "rm-baduser: expected non-zero exit"; fi
grep -qi "invalid username" "$OUT7B" \
  || fail "rm-baduser: expected 'invalid username' (got: $(head -1 "$OUT7B"))"

# --- 8-11. Auth-gated mutation tests ---
#
# Require a primary owner token (NPKG_TEST_TOKEN) and a secondary user
# (NPKG_TEST_TOKEN_B + NPKG_TEST_USER_B) plus a package owned by the primary
# (NPKG_TEST_PKG). The secondary user must NOT already own the package on
# entry; on success we leave the package state exactly as we found it.
if [ -n "${NPKG_TEST_TOKEN:-}" ] \
   && [ -n "${NPKG_TEST_TOKEN_B:-}" ] \
   && [ -n "${NPKG_TEST_USER_B:-}" ] \
   && [ -n "${NPKG_TEST_PKG:-}" ]; then

  PKG="$NPKG_TEST_PKG"
  USER_B="$NPKG_TEST_USER_B"

  # 8. Add USER_B as a co-owner.
  OUT8="$SCRATCH/add.out"
  "$INTERPRETER" "$NPKG" owner add "$PKG" "$USER_B" \
    --registry "$REGISTRY" --token "$NPKG_TEST_TOKEN" >"$OUT8" 2>&1
  RC8=$?
  if [ "$RC8" != "0" ]; then
    cat "$OUT8" >&2
    fail "add: expected exit 0 adding '$USER_B' to '$PKG'"
  fi

  # ls should now show USER_B (best-effort grep; the line format is
  # "  <username>  (since ...)" or "  <username>").
  OUT8L="$SCRATCH/add-ls.out"
  "$INTERPRETER" "$NPKG" owner ls "$PKG" --registry "$REGISTRY" >"$OUT8L" 2>&1
  grep -qE "^  $USER_B(\$|  )" "$OUT8L" \
    || fail "add: expected ls to list '$USER_B' after add (got: $(sed -n '1,8p' "$OUT8L"))"

  # 9. Adding the same user again is idempotent: 2xx + "already" message.
  OUT9="$SCRATCH/add-again.out"
  "$INTERPRETER" "$NPKG" owner add "$PKG" "$USER_B" \
    --registry "$REGISTRY" --token "$NPKG_TEST_TOKEN" >"$OUT9" 2>&1
  RC9=$?
  if [ "$RC9" != "0" ]; then
    cat "$OUT9" >&2
    fail "add-again: expected exit 0 on idempotent add"
  fi
  grep -qi "already" "$OUT9" \
    || fail "add-again: expected 'already' in message (got: $(head -1 "$OUT9"))"

  # 10. Co-owner can remove itself (using TOKEN_B).
  OUT10="$SCRATCH/rm-self.out"
  "$INTERPRETER" "$NPKG" owner rm "$PKG" "$USER_B" \
    --registry "$REGISTRY" --token "$NPKG_TEST_TOKEN_B" >"$OUT10" 2>&1
  RC10=$?
  if [ "$RC10" != "0" ]; then
    cat "$OUT10" >&2
    fail "rm-self: expected co-owner '$USER_B' to be able to remove itself"
  fi

  # 11. The remaining owner cannot be removed (last-owner refusal -> 409).
  # We don't know the primary username, but we can ls and pull the first
  # listed owner; with USER_B gone there should be exactly one entry.
  REMAINING="$(grep -E '^  [A-Za-z0-9._-]+' "$OUT8L" | head -1 \
              | sed -E 's/^  ([A-Za-z0-9._-]+).*/\1/' || true)"
  # Re-read ls after removing USER_B so we pick the real sole owner.
  "$INTERPRETER" "$NPKG" owner ls "$PKG" --registry "$REGISTRY" \
    >"$SCRATCH/post-rm-ls.out" 2>&1
  SOLE="$(grep -E '^  [A-Za-z0-9._-]+' "$SCRATCH/post-rm-ls.out" | head -1 \
          | sed -E 's/^  ([A-Za-z0-9._-]+).*/\1/' || true)"
  if [ -n "$SOLE" ]; then
    OUT11="$SCRATCH/rm-last.out"
    "$INTERPRETER" "$NPKG" owner rm "$PKG" "$SOLE" \
      --registry "$REGISTRY" --token "$NPKG_TEST_TOKEN" >"$OUT11" 2>&1
    RC11=$?
    if [ "$RC11" = "0" ]; then
      cat "$OUT11" >&2
      fail "rm-last: expected non-zero exit removing sole owner '$SOLE'"
    fi
    grep -qiE "last owner|cannot remove" "$OUT11" \
      || fail "rm-last: expected 'last owner' / 'cannot remove' message (got: $(head -1 "$OUT11"))"
  else
    echo "[note] npkg owner smoke: could not detect sole owner, skipping last-owner refusal check"
  fi
else
  echo "[note] npkg owner smoke: auth tests skipped (set NPKG_TEST_TOKEN, NPKG_TEST_TOKEN_B, NPKG_TEST_USER_B, NPKG_TEST_PKG to enable)"
fi

if [ "$FAIL" -ne 0 ]; then
  echo "npkg owner smoke: FAILED" >&2
  exit 1
fi

echo "npkg owner smoke: PASS"
exit 0
