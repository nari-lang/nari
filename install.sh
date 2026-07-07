#!/bin/sh
# install.sh \u2014 Nari one-line installer.
#
# Usage (default, prompts for npkg):
#  curl -fsSL https://raw.githubusercontent.com/wearrrrr/Nari/main/install.sh | sh
#
# Non-interactive variants:
#  curl -fsSL .../install.sh | sh -s -- --with-npkg
#  curl -fsSL .../install.sh | sh -s -- --no-npkg
#  curl -fsSL .../install.sh | sh -s -- --version v0.1.2
#  NARI_HOME=/opt/nari curl -fsSL .../install.sh | sh
#
# The script:
#  1. Detects (os, arch) -> linux|macos x x86_64|aarch64.
#  2. Resolves a release tag (--version, $NARI_VERSION, or "latest").
#  3. Downloads nari-<tag>-<os>-<arch>.tar.gz and verifies sha256
#  against the same release's SHASUMS256.txt.
#  4. Extracts into $NARI_HOME (default ~/.nari): bin/{nari,naric} and
#  \u2014 if the user opts in \u2014 bin/npkg + lib/npkg/.
#  5. Edits ~/.bashrc / ~/.zshrc to put $NARI_HOME/bin on PATH if it
#  isn't already, and prints fish/nu instructions.
#
# POSIX sh (dash-safe). Does not require sudo: everything lands under
# $NARI_HOME, which defaults to a user-writable path.

set -eu

# -------------------------- defaults / parsing --------------------------

REPO="wearrrrr/Nari"
NARI_HOME="${NARI_HOME:-$HOME/.nari}"
VERSION="${NARI_VERSION:-}"
NPKG_CHOICE=""  # empty = prompt, "yes", or "no"
QUIET=0
FORCE=0

usage() {
  cat <<EOF
Nari installer

Usage:
  install.sh [--version <tag>] [--with-npkg|--no-npkg] [--prefix <dir>]
  [--force] [--quiet] [--help]

Options:
  --version <tag>  Install a specific release tag (default: latest).
  --with-npkg  Install the npkg package manager without prompting.
  --no-npkg  Skip npkg without prompting.
  --prefix <dir>  Install into <dir> instead of \$NARI_HOME (~/.nari).
  --force  Overwrite an existing install without confirming.
  --quiet  Suppress non-error output.
  --help  Show this message.

Environment:
  NARI_HOME  Same as --prefix.
  NARI_VERSION  Same as --version.

The installer never uses sudo. To uninstall:
  curl -fsSL https://raw.githubusercontent.com/wearrrrr/Nari/main/release/uninstall.sh | sh
  # add --purge to also remove ~/.nari/{cache,store,npkg} (credentials + caches)
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
  --version) VERSION="$2"; shift 2 ;;
  --version=*) VERSION="${1#*=}"; shift ;;
  --with-npkg) NPKG_CHOICE="yes"; shift ;;
  --no-npkg)  NPKG_CHOICE="no";  shift ;;
  --prefix) NARI_HOME="$2"; shift 2 ;;
  --prefix=*) NARI_HOME="${1#*=}"; shift ;;
  --force) FORCE=1; shift ;;
  --quiet) QUIET=1; shift ;;
  --help|-h) usage; exit 0 ;;
  *) printf 'install.sh: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

# -------------------------- helpers --------------------------

log() { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }
err() { printf 'install.sh: %s\n' "$*" >&2; }
die() { err "$*"; exit 1; }

# Pick the first available downloader. Both curl and wget are nearly
# universal on linux+macos; we don't fall back further (e.g. python -m http)
# because most users will have at least one.
download() {
  # download <url> <output_path>
  if command -v curl >/dev/null 2>&1; then
  curl --proto '=https' --tlsv1.2 -fsSL "$1" -o "$2"
  elif command -v wget >/dev/null 2>&1; then
  wget -q -O "$2" "$1"
  else
  die "neither curl nor wget is available; cannot download $1"
  fi
}

# Read a single line from /dev/tty even when stdin is piped (curl|sh).
# Returns empty if no tty is attached (the caller treats that as "use
# the default").
read_tty() {
  if [ -r /dev/tty ]; then
  # shellcheck disable=SC2162
  read ans </dev/tty || ans=""
  printf '%s' "$ans"
  else
  printf ''
  fi
}

sha256_of() {
  # POSIX-ish: prefer shasum (mac default) then sha256sum (linux default).
  if command -v shasum >/dev/null 2>&1; then
  shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$1" | awk '{print $1}'
  else
  die "no sha256 tool available (need shasum or sha256sum)"
  fi
}

# -------------------------- platform detect --------------------------

detect_os() {
  uname_s=$(uname -s)
  case "$uname_s" in
  Linux)  printf 'linux'  ;;
  Darwin) printf 'macos'  ;;
  *) die "unsupported OS: $uname_s (linux and macos only)" ;;
  esac
}

detect_arch() {
  uname_m=$(uname -m)
  case "$uname_m" in
  x86_64|amd64) printf 'x86_64' ;;
  aarch64|arm64) printf 'aarch64' ;;
  *) die "unsupported architecture: $uname_m (x86_64 and aarch64 only)" ;;
  esac
}

OS=$(detect_os)
ARCH=$(detect_arch)
log "Detected platform: ${OS}-${ARCH}"

# -------------------------- resolve tag --------------------------

# `latest` resolves via the redirect on /releases/latest. We follow the
# redirect to get the actual tag and then download the asset directly so
# the URL is stable in the log.
resolve_latest() {
  # GitHub redirects /releases/latest -> /releases/tag/<tag>. We use a
  # HEAD request to extract that tag without downloading anything.
  url="https://github.com/${REPO}/releases/latest"
  if command -v curl >/dev/null 2>&1; then
  final=$(curl --proto '=https' --tlsv1.2 -fsSLI -o /dev/null -w '%{url_effective}' "$url") \
  || die "could not query latest release at $url"
  elif command -v wget >/dev/null 2>&1; then
  # wget doesn't print the final URL as cleanly; fall back to grepping
  # the API response, which works without auth for public repos.
  api="https://api.github.com/repos/${REPO}/releases/latest"
  final=$(wget -q -O - "$api" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)
  [ -n "$final" ] || die "could not parse latest release from $api"
  printf '%s' "$final"
  return
  else
  die "neither curl nor wget is available"
  fi
  # Strip everything before the tag.
  printf '%s' "${final##*/}"
}

if [ -z "$VERSION" ]; then
  VERSION=$(resolve_latest)
fi
case "$VERSION" in
  v*) : ;;
  *) VERSION="v${VERSION}" ;;
esac
log "Installing Nari ${VERSION}"

# -------------------------- download + verify --------------------------

ASSET="nari-${VERSION}-${OS}-${ARCH}.tar.gz"
BASE_URL="https://github.com/${REPO}/releases/download/${VERSION}"
TARBALL_URL="${BASE_URL}/${ASSET}"
SHASUMS_URL="${BASE_URL}/SHASUMS256.txt"

TMPDIR=$(mktemp -d 2>/dev/null || mktemp -d -t nari-install)
trap 'rm -rf "$TMPDIR"' EXIT INT TERM

log "Downloading ${TARBALL_URL}"
download "$TARBALL_URL" "$TMPDIR/$ASSET" \
  || die "download failed: $TARBALL_URL (does the release exist for this platform?)"

log "Verifying checksum"
download "$SHASUMS_URL" "$TMPDIR/SHASUMS256.txt" \
  || die "could not fetch $SHASUMS_URL"

actual=$(sha256_of "$TMPDIR/$ASSET")
expected=$(awk -v f="$ASSET" '$2 == f || $2 == "*"f {print $1; exit}' "$TMPDIR/SHASUMS256.txt")
if [ -z "$expected" ]; then
  die "no checksum entry for $ASSET in SHASUMS256.txt"
fi
if [ "$actual" != "$expected" ]; then
  die "checksum mismatch for $ASSET (expected $expected, got $actual)"
fi
log "Checksum OK"

# -------------------------- extract --------------------------

# Use $NARI_HOME as the install prefix. The tarball top dir is
# nari-<version>-<os>-<arch>/, so we strip-components=1 to land
# bin/ + lib/ directly under $NARI_HOME.
if [ -d "$NARI_HOME" ] && [ "$FORCE" -ne 1 ]; then
  if [ -x "$NARI_HOME/bin/nari" ]; then
  existing=$("$NARI_HOME/bin/nari" --version 2>/dev/null || echo unknown)
  log "Existing install at $NARI_HOME (${existing}); replacing."
  fi
fi
mkdir -p "$NARI_HOME/bin" "$NARI_HOME/lib"
# Remove the previous nari + naric so an older symlink/file is replaced
# atomically. We leave lib/npkg alone for now and overwrite below only
# if the user keeps npkg installed.
rm -f "$NARI_HOME/bin/nari" "$NARI_HOME/bin/naric"

log "Extracting into ${NARI_HOME}"
tar -xzf "$TMPDIR/$ASSET" -C "$TMPDIR"
SRC="$TMPDIR/nari-${VERSION}-${OS}-${ARCH}"
[ -d "$SRC" ] || die "tarball did not contain expected directory: $SRC"

cp "$SRC/bin/nari"  "$NARI_HOME/bin/nari"
cp "$SRC/bin/naric" "$NARI_HOME/bin/naric"
chmod +x "$NARI_HOME/bin/nari" "$NARI_HOME/bin/naric"

# -------------------------- npkg prompt --------------------------

prompt_npkg() {
  # Default: yes on a TTY, no when piped without explicit flag. The
  # "curl | sh" path *does* have /dev/tty even though stdin is the
  # pipe, so we still ask interactively in that case.
  if [ -r /dev/tty ]; then
  printf 'Install npkg (the Nari package manager)? [Y/n] ' >/dev/tty
  ans=$(read_tty)
  case "$ans" in
  n|N|no|NO) NPKG_CHOICE="no" ;;
  *)  NPKG_CHOICE="yes" ;;
  esac
  else
  NPKG_CHOICE="no"
  fi
}

if [ -z "$NPKG_CHOICE" ]; then
  prompt_npkg
fi

if [ "$NPKG_CHOICE" = "yes" ]; then
  log "Installing npkg"
  rm -rf "$NARI_HOME/lib/npkg"
  mkdir -p "$NARI_HOME/lib/npkg"
  cp -R "$SRC/lib/npkg/." "$NARI_HOME/lib/npkg/"
  # The bundled bin/npkg wrapper is relative-resolving, so a direct copy
  # works whether the user keeps $NARI_HOME or moves it later.
  cp "$SRC/bin/npkg" "$NARI_HOME/bin/npkg"
  chmod +x "$NARI_HOME/bin/npkg"
else
  log "Skipping npkg (run install.sh again with --with-npkg to add it)"
  # If a previous install had npkg, remove it cleanly so the user's
  # choice this run is the source of truth.
  rm -f "$NARI_HOME/bin/npkg"
  rm -rf "$NARI_HOME/lib/npkg"
fi

# -------------------------- PATH edit --------------------------

# Append a single PATH line to the user's shell rc if it isn't already
# present. We key off the literal NARI_HOME path so re-running the
# installer doesn't duplicate the line.
add_path_to_rc() {
  rc="$1"
  [ -f "$rc" ] || return 0
  if grep -Fq "$NARI_HOME/bin" "$rc" 2>/dev/null; then
  return 0
  fi
  {
  printf '\n# Added by Nari installer\n'
  printf 'export PATH="%s/bin:$PATH"\n' "$NARI_HOME"
  } >> "$rc"
  log "Added $NARI_HOME/bin to PATH in $rc"
}

shell_name=$(basename "${SHELL:-/bin/sh}")
case "$shell_name" in
  bash)
  add_path_to_rc "$HOME/.bashrc"
  [ -f "$HOME/.bash_profile" ] && add_path_to_rc "$HOME/.bash_profile"
  ;;
  zsh)
  add_path_to_rc "$HOME/.zshrc"
  ;;
  sh|dash|ash)
  add_path_to_rc "$HOME/.profile"
  ;;
  *)
  log "Your shell ($shell_name) wasn't auto-configured."
  log "Add this line to your shell's startup file:"
  log "  export PATH=\"$NARI_HOME/bin:\$PATH\""
  ;;
esac

# -------------------------- done --------------------------

log ""
log "Nari ${VERSION} installed to ${NARI_HOME}"
log "  bin: ${NARI_HOME}/bin"
if [ "$NPKG_CHOICE" = "yes" ]; then
  log "  npkg ready: try 'npkg search <query>' once your shell reloads"
fi
log ""
log "Open a new terminal (or 'source' your shell rc) and run:"
log "  nari --help"
