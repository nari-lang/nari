#!/usr/bin/env bash
#
# npkg publish polish smoke test.
#
# Verifies the publish-path validators and reporting (npkg-frontend/npkg/lib/publish.nari)
# without requiring a running registry. All scenarios use --dry-run with
# --registry http://localhost:0 so the upload step is never reached.
#
# Coverage:
#   1. Clean publish reports archive size / file count / short sha256.
#   2. Invalid package name -> validator throws and exits non-zero.
#   3. Invalid version string -> validator throws and exits non-zero.
#   4. Dirty git working tree -> publish refuses with helpful message.
#   5. Dirty git + --allow-dirty -> publish proceeds.
#   6. .npkgignore excludes matching files from the archive.
#   7. Default ignore patterns exclude .git/, build/ and *.log.
#   8. Missing recommended fields (description/license) emit warnings.
#
# Exits 0 on success, 1 on first failure. Quiet on success.

set -uo pipefail
# Note: -e is intentionally NOT set. Several scenarios deliberately invoke
# `npkg publish` with bad inputs and expect non-zero exits; the captured
# `run_publish` helper records each exit code in $out.exit for assertions.

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
NPKG="$REPO_ROOT/npkg-frontend/npkg/main.nari"
INTERPRETER="$REPO_ROOT/build/release/nari"

if [ ! -f "$NPKG" ]; then
  echo "[skip] npkg entry script missing: $NPKG"
  exit 0
fi
if [ ! -x "$INTERPRETER" ]; then
  echo "[skip] interpreter missing: $INTERPRETER"
  exit 0
fi

SCRATCH="$(mktemp -d -t npkg-publish-polish-XXXXXX)"
trap 'rm -rf "$SCRATCH"' EXIT

FAIL=0
fail() { echo "[fail] $*" >&2; FAIL=1; }

# Initialise a self-contained git-tracked package at $1, with manifest
# fields overridden via additional KEY=VAL args (TOML format applied as-is).
init_pkg() {
  local dir="$1"; shift
  mkdir -p "$dir/src"
  cat >"$dir/src/main.nari" <<'NARI'
export func hello() { return "hi"; }
NARI
  cat >"$dir/nari.toml" <<TOML
packageFormat = 1
name = "demo-pkg"
version = "0.1.0"
description = "demo package for smoke tests"
license = "MIT"

[exports]
"." = "src/main.nari"
TOML
  # Apply overrides: each arg is "key=value" -> replaces matching `key = ...` line.
  local kv k v
  for kv in "$@"; do
    k="${kv%%=*}"
    v="${kv#*=}"
    if grep -q "^${k} = " "$dir/nari.toml"; then
      # Use a different sed delimiter to allow special chars in values.
      sed -i "s|^${k} = .*|${k} = ${v}|" "$dir/nari.toml"
    else
      printf '%s = %s\n' "$k" "$v" >>"$dir/nari.toml"
    fi
  done
  ( cd "$dir" && git init -q && git add -A && \
    git -c user.email=t@t -c user.name=t commit -q -m init >/dev/null )
}

run_publish() {
  # Usage: run_publish DIR OUTFILE [extra-flags...]
  local dir="$1" out="$2"; shift 2
  ( cd "$dir" && "$INTERPRETER" "$NPKG" publish "$dir" --dry-run \
      --registry "http://localhost:0" --token dummy "$@" ) \
    >"$out" 2>&1 || true
  # Capture exit code via a follow-up run when needed by the caller. We
  # don't want set -e to abort here on expected failures, hence "|| true".
  ( cd "$dir" && "$INTERPRETER" "$NPKG" publish "$dir" --dry-run \
      --registry "http://localhost:0" --token dummy "$@" >/dev/null 2>&1 )
  echo $? >"$out.exit"
}

# ---------------------------------------------------------------------------
# 1. Clean publish prints archive metadata.
# ---------------------------------------------------------------------------
PKG1="$SCRATCH/clean"
init_pkg "$PKG1"
OUT1="$SCRATCH/clean.out"
run_publish "$PKG1" "$OUT1"
if [ "$(cat "$OUT1.exit")" != "0" ]; then
  cat "$OUT1" >&2
  fail "clean publish: expected exit 0, got $(cat "$OUT1.exit")"
fi
grep -q "Creating archive" "$OUT1" || fail "clean: missing 'Creating archive' progress line"
grep -qE "Archive: [0-9]+ (B|KiB|MiB), [0-9]+ files, sha256=[0-9a-f]{16}" "$OUT1" \
  || fail "clean: missing 'Archive: <size>, <n> files, sha256=...' line"
grep -q "Dry run: archive prepared at" "$OUT1" || fail "clean: missing dry-run summary"

# ---------------------------------------------------------------------------
# 2. Invalid package name -> validator throws.
# ---------------------------------------------------------------------------
PKG2="$SCRATCH/badname"
init_pkg "$PKG2" 'name="Bad_Name"'
OUT2="$SCRATCH/badname.out"
run_publish "$PKG2" "$OUT2"
if [ "$(cat "$OUT2.exit")" = "0" ]; then
  cat "$OUT2" >&2
  fail "badname: expected non-zero exit"
fi
grep -q "Package name must be lowercase" "$OUT2" \
  || fail "badname: missing helpful error (got: $(head -3 "$OUT2" | tr '\n' ' '))"

# ---------------------------------------------------------------------------
# 3. Invalid version string -> validator throws.
# ---------------------------------------------------------------------------
PKG3="$SCRATCH/badver"
init_pkg "$PKG3" 'version=".1.0"'
OUT3="$SCRATCH/badver.out"
run_publish "$PKG3" "$OUT3"
if [ "$(cat "$OUT3.exit")" = "0" ]; then
  cat "$OUT3" >&2
  fail "badver: expected non-zero exit"
fi
grep -qiE "version" "$OUT3" || fail "badver: error message should mention 'version'"

# ---------------------------------------------------------------------------
# 4. Dirty git working tree -> publish refuses.
# ---------------------------------------------------------------------------
PKG4="$SCRATCH/dirty"
init_pkg "$PKG4"
echo "untracked" >"$PKG4/extra.nari"
OUT4="$SCRATCH/dirty.out"
run_publish "$PKG4" "$OUT4"
if [ "$(cat "$OUT4.exit")" = "0" ]; then
  cat "$OUT4" >&2
  fail "dirty: expected non-zero exit (publish should refuse dirty tree)"
fi
grep -qiE "dirty|uncommit|allow-dirty" "$OUT4" \
  || fail "dirty: error should mention dirty/uncommitted/allow-dirty"
grep -qE "Uncommitted files:" "$OUT4" \
  || fail "dirty: error should list 'Uncommitted files:' header"
grep -qE "extra\.nari" "$OUT4" \
  || fail "dirty: error should name the dirty file (extra.nari)"

# ---------------------------------------------------------------------------
# 5. Dirty git + --allow-dirty -> proceeds.
# ---------------------------------------------------------------------------
OUT5="$SCRATCH/dirty-allow.out"
run_publish "$PKG4" "$OUT5" --allow-dirty
if [ "$(cat "$OUT5.exit")" != "0" ]; then
  cat "$OUT5" >&2
  fail "dirty-allow: expected exit 0 with --allow-dirty"
fi
grep -q "Dry run: archive prepared at" "$OUT5" \
  || fail "dirty-allow: missing dry-run summary"

# ---------------------------------------------------------------------------
# 6. .npkgignore excludes matching files.
# ---------------------------------------------------------------------------
PKG6="$SCRATCH/ignore"
init_pkg "$PKG6"
echo "secret" >"$PKG6/secrets.txt"
echo "keep" >"$PKG6/keep.txt"
cat >"$PKG6/.npkgignore" <<'IGN'
secrets.txt
IGN
( cd "$PKG6" && git add -A && git -c user.email=t@t -c user.name=t commit -q -m add )
OUT6="$SCRATCH/ignore.out"
run_publish "$PKG6" "$OUT6"
if [ "$(cat "$OUT6.exit")" != "0" ]; then
  cat "$OUT6" >&2
  fail "ignore: expected exit 0"
fi
ARCH6="$HOME/.nari/cache/npkg-publish/demo-pkg-0.1.0/package.tar.gz"
if [ ! -f "$ARCH6" ]; then
  fail "ignore: archive missing at $ARCH6"
else
  if tar -tzf "$ARCH6" | grep -qE '(^|/)secrets\.txt$'; then
    fail "ignore: secrets.txt should have been excluded by .npkgignore"
  fi
  if ! tar -tzf "$ARCH6" | grep -qE '(^|/)keep\.txt$'; then
    fail "ignore: keep.txt should be in the archive"
  fi
fi

# ---------------------------------------------------------------------------
# 7. Default ignore patterns exclude .git/, build/, and *.log.
# ---------------------------------------------------------------------------
PKG7="$SCRATCH/defaults"
init_pkg "$PKG7"
mkdir -p "$PKG7/build"
echo "obj" >"$PKG7/build/out.o"
echo "log line" >"$PKG7/debug.log"
( cd "$PKG7" && git add -A && git -c user.email=t@t -c user.name=t commit -q -m extra )
OUT7="$SCRATCH/defaults.out"
run_publish "$PKG7" "$OUT7"
if [ "$(cat "$OUT7.exit")" != "0" ]; then
  cat "$OUT7" >&2
  fail "defaults: expected exit 0"
fi
ARCH7="$HOME/.nari/cache/npkg-publish/demo-pkg-0.1.0/package.tar.gz"
if [ -f "$ARCH7" ]; then
  ENTRIES="$(tar -tzf "$ARCH7")"
  if echo "$ENTRIES" | grep -qE '(^|/)\.git(/|$)'; then
    fail "defaults: .git/ should have been excluded"
  fi
  if echo "$ENTRIES" | grep -qE '(^|/)build(/|$)'; then
    fail "defaults: build/ should have been excluded"
  fi
  if echo "$ENTRIES" | grep -qE '(^|/)debug\.log$'; then
    fail "defaults: *.log should have been excluded"
  fi
fi

# ---------------------------------------------------------------------------
# 8. Missing recommended fields warn (but don't fail).
# ---------------------------------------------------------------------------
PKG8="$SCRATCH/nodesc"
mkdir -p "$PKG8/src"
cat >"$PKG8/src/main.nari" <<'NARI'
export func hello() { return "hi"; }
NARI
cat >"$PKG8/nari.toml" <<'TOML'
packageFormat = 1
name = "demo-pkg"
version = "0.1.0"

[exports]
"." = "src/main.nari"
TOML
( cd "$PKG8" && git init -q && git add -A && \
  git -c user.email=t@t -c user.name=t commit -q -m init )
OUT8="$SCRATCH/nodesc.out"
run_publish "$PKG8" "$OUT8"
if [ "$(cat "$OUT8.exit")" != "0" ]; then
  cat "$OUT8" >&2
  fail "nodesc: expected exit 0 (warnings should not fail)"
fi
grep -qiE "warn(ing)?:.*description|description.*recommend" "$OUT8" \
  || fail "nodesc: expected a 'description' warning"
grep -qiE "warn(ing)?:.*license|license.*recommend" "$OUT8" \
  || fail "nodesc: expected a 'license' warning"

# ---------------------------------------------------------------------------
if [ "$FAIL" -ne 0 ]; then
  echo "npkg publish polish: FAILED" >&2
  exit 1
fi

echo "npkg publish polish: PASS"
exit 0
