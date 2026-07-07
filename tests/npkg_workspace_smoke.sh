#!/usr/bin/env bash
#
# npkg workspace smoke test.
#
# Verifies the workspace pipeline end-to-end *without* a running registry:
#   1. `npkg install` from a virtual-root workspace materializes both members
#      into the store, writes lockfile v2 with [workspace].members, and lists
#      cross-member dependency as source = "workspace".
#   2. `npkg publish --workspace --dry-run` stages every member, rewrites
#      workspace-only deps to pinned versions, and reports success per member.
#
# Exits 0 on success, 1 on first failure. Quiet on success.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE_SRC="$REPO_ROOT/npkg-frontend/npkg/fixtures/workspace"
NPKG="$REPO_ROOT/npkg-frontend/npkg/main.nari"

if [ ! -d "$FIXTURE_SRC" ]; then
  echo "[skip] workspace fixture missing: $FIXTURE_SRC"
  exit 0
fi
if [ ! -f "$NPKG" ]; then
  echo "[skip] npkg entry script missing: $NPKG"
  exit 0
fi

# Use a private scratch dir so the suite never mutates the committed fixture
# and never collides with another concurrent test runner.
SCRATCH="$(mktemp -d -t npkg-ws-smoke-XXXXXX)"
trap 'rm -rf "$SCRATCH"' EXIT

cp -a "$FIXTURE_SRC/." "$SCRATCH/"

FAIL=0
fail() { echo "[fail] $*" >&2; FAIL=1; }
require_contains() {
  # $1 = file, $2 = needle, $3 = description
  if ! grep -qF -- "$2" "$1"; then
    fail "$3: '$2' not found in $1"
  fi
}

# 1) install
INSTALL_OUT="$SCRATCH/install.out"
if ! ( cd "$SCRATCH" && "$NPKG" install ) >"$INSTALL_OUT" 2>&1; then
  cat "$INSTALL_OUT" >&2
  fail "install: npkg install exited non-zero"
fi

require_contains "$INSTALL_OUT" "Installed @demo/lib-a@1.0.0" "install: lib-a not materialized"
require_contains "$INSTALL_OUT" "Installed @demo/app-b@0.1.0" "install: app-b not materialized"

LOCK="$SCRATCH/nari.lock.toml"
if [ ! -f "$LOCK" ]; then
  fail "install: lockfile not written"
else
  require_contains "$LOCK" "lockfileVersion = 2" "lockfile: missing v2 marker"
  require_contains "$LOCK" "[workspace]" "lockfile: missing [workspace] table"
  require_contains "$LOCK" "@demo/lib-a@1.0.0" "lockfile: lib-a not listed as member"
  require_contains "$LOCK" "@demo/app-b@0.1.0" "lockfile: app-b not listed as member"
  require_contains "$LOCK" 'source = "workspace"' "lockfile: missing source = workspace"
fi

# 2) publish --workspace --dry-run (no server required)
PUBLISH_OUT="$SCRATCH/publish.out"
if ! ( cd "$SCRATCH" && "$NPKG" publish --workspace --dry-run --registry "http://localhost:0" ) \
     >"$PUBLISH_OUT" 2>&1; then
  cat "$PUBLISH_OUT" >&2
  fail "publish: dry-run exited non-zero"
fi

require_contains "$PUBLISH_OUT" "Publishing workspace (2 members)" "publish: header wrong/missing"
require_contains "$PUBLISH_OUT" "(dry-run) Publishing @demo/lib-a@1.0.0" "publish: lib-a not staged"
require_contains "$PUBLISH_OUT" "(dry-run) Publishing @demo/app-b@0.1.0" "publish: app-b not staged"
require_contains "$PUBLISH_OUT" "Rewrote workspace deps to pinned versions" \
  "publish: app-b dep rewrite did not run"
require_contains "$PUBLISH_OUT" "Dry run complete: 2 member(s) staged." \
  "publish: summary missing"

# Spot-check the rewritten manifest inside app-b's staged archive: the
# `workspace = true` table syntax must have been replaced with a plain
# pinned version, otherwise downstream registry consumers would fail
# because they can't resolve a workspace member.
APPB_ARCHIVE="$HOME/.nari/cache/npkg-publish/demo-app-b-0.1.0/package.tar.gz"
if [ -f "$APPB_ARCHIVE" ]; then
  # Try both with and without the leading ./ since libarchive emits flat
  # entries while GNU tar's -czf . prefixes them.
  REWRITTEN="$(tar -xzOf "$APPB_ARCHIVE" nari.toml 2>/dev/null || tar -xzOf "$APPB_ARCHIVE" ./nari.toml 2>/dev/null || true)"
  if echo "$REWRITTEN" | grep -q "workspace = true"; then
    fail "publish: app-b archive still contains 'workspace = true'"
  fi
  if ! echo "$REWRITTEN" | grep -qF '"@demo/lib-a" = "1.0.0"'; then
    fail "publish: app-b archive missing pinned '@demo/lib-a = 1.0.0'"
  fi
else
  fail "publish: app-b archive missing at $APPB_ARCHIVE"
fi

if [ "$FAIL" -ne 0 ]; then
  echo "npkg workspace smoke: FAILED" >&2
  exit 1
fi

echo "npkg workspace smoke: PASS"
exit 0
