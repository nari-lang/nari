#!/bin/sh
# Nari installer.
#   curl -fsSL https://raw.githubusercontent.com/nari-lang/nari/main/install.sh | sh
set -eu

REPO="nari-lang/nari"
NPKG_REPO="nari-lang/npkg"
NARI_HOME="${NARI_HOME:-$HOME/.nari}"
VERSION="${NARI_VERSION:-}"
NPKG_VERSION="${NPKG_VERSION:-}"
NPKG_CHOICE=""
QUIET=0

usage() {
    cat <<EOF
Nari installer

Usage:
  install.sh [--version <tag>] [--with-npkg|--no-npkg] [--prefix <dir>]
             [--quiet] [--help]

Options:
  --version <tag>   Install a specific release tag (default: latest).
  --with-npkg       Install the npkg package manager without prompting.
  --no-npkg         Skip npkg without prompting.
  --npkg-version <tag>  Install a specific npkg release (default: latest).
  --prefix <dir>    Install into <dir> instead of \$NARI_HOME (~/.nari).
  --quiet           Suppress non-error output.
  --help            Show this message.

Environment:
  NARI_HOME         Same as --prefix.
  NARI_VERSION      Same as --version.
  NPKG_VERSION      Same as --npkg-version.

The installer never uses sudo. To uninstall:
  curl -fsSL https://raw.githubusercontent.com/nari-lang/nari/main/uninstall.sh | sh
EOF
}

log() { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }
err() { printf 'install.sh: %s\n' "$*" >&2; }
die() { err "$*"; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#*=}"; shift ;;
        --with-npkg) NPKG_CHOICE="yes"; shift ;;
        --no-npkg) NPKG_CHOICE="no"; shift ;;
        --npkg-version) NPKG_VERSION="$2"; shift 2 ;;
        --npkg-version=*) NPKG_VERSION="${1#*=}"; shift ;;
        --prefix) NARI_HOME="$2"; shift 2 ;;
        --prefix=*) NARI_HOME="${1#*=}"; shift ;;
        --force) shift ;;
        --quiet) QUIET=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) err "unknown option: $1"; usage >&2; exit 2 ;;
    esac
done

if command -v curl >/dev/null 2>&1; then
    HAVE_CURL=1
elif command -v wget >/dev/null 2>&1; then
    HAVE_CURL=0
else
    die "neither curl nor wget is available"
fi

download() {
    if [ "$HAVE_CURL" -eq 1 ]; then
        curl --proto '=https' --tlsv1.2 -fsSL "$1" -o "$2"
    else
        wget -q -O "$2" "$1"
    fi
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        die "no sha256 tool available (need sha256sum or shasum)"
    fi
}

case "$(uname -s)" in
    Linux) OS=linux ;;
    Darwin) OS=macos ;;
    *) die "unsupported OS: $(uname -s) (linux and macos only)" ;;
esac
case "$(uname -m)" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=arm64 ;;
    *) die "unsupported architecture: $(uname -m) (x86_64 and arm64 only)" ;;
esac
log "Detected platform: ${OS}-${ARCH}"

resolve_latest() {
    repo="$1"
    if [ "$HAVE_CURL" -eq 1 ]; then
        final=$(curl --proto '=https' --tlsv1.2 -fsSLI -o /dev/null -w '%{url_effective}' \
            "https://github.com/${repo}/releases/latest") || return 1
        printf '%s' "${final##*/}"
    else
        wget -q -O - "https://api.github.com/repos/${repo}/releases/latest" \
            | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1
    fi
}

if [ -z "$VERSION" ]; then
    VERSION=$(resolve_latest "$REPO") || VERSION=""
    case "$VERSION" in
        ""|releases) die "no published releases found for ${REPO}" ;;
    esac
fi
case "$VERSION" in
    v*) ;;
    *) VERSION="v${VERSION}" ;;
esac
log "Installing Nari ${VERSION}"

ASSET="nari-${VERSION}-${OS}-${ARCH}.tar.gz"
BASE_URL="https://github.com/${REPO}/releases/download/${VERSION}"

tmpdir=$(mktemp -d 2>/dev/null || mktemp -d -t nari-install)
trap 'rm -rf "$tmpdir"' EXIT INT TERM

log "Downloading ${BASE_URL}/${ASSET}"
download "${BASE_URL}/${ASSET}" "$tmpdir/$ASSET" \
    || die "download failed: ${BASE_URL}/${ASSET} (does the release exist for this platform?)"

log "Verifying checksum"
download "${BASE_URL}/SHASUMS256.txt" "$tmpdir/SHASUMS256.txt" \
    || die "could not fetch ${BASE_URL}/SHASUMS256.txt"
expected=$(awk -v f="$ASSET" '$2 == f || $2 == "*"f {print $1; exit}' "$tmpdir/SHASUMS256.txt")
[ -n "$expected" ] || die "no checksum entry for $ASSET in SHASUMS256.txt"
actual=$(sha256_of "$tmpdir/$ASSET")
[ "$actual" = "$expected" ] || die "checksum mismatch for $ASSET (expected $expected, got $actual)"
log "Checksum OK"

tar -xzf "$tmpdir/$ASSET" -C "$tmpdir"
SRC="$tmpdir/nari-${VERSION}-${OS}-${ARCH}"
[ -d "$SRC" ] || die "tarball did not contain expected directory: nari-${VERSION}-${OS}-${ARCH}"

if [ -x "$NARI_HOME/bin/nari" ]; then
    existing=$("$NARI_HOME/bin/nari" --version 2>/dev/null || echo unknown)
    log "Replacing existing install at $NARI_HOME (${existing})"
fi

log "Installing into ${NARI_HOME}"
mkdir -p "$NARI_HOME/bin"
rm -f "$NARI_HOME/bin/nari" "$NARI_HOME/bin/naric"
cp "$SRC/bin/nari" "$NARI_HOME/bin/nari"
cp "$SRC/bin/naric" "$NARI_HOME/bin/naric"
chmod +x "$NARI_HOME/bin/nari" "$NARI_HOME/bin/naric"

# npkg lives in its own repo (pure Nari source, one tarball for all
# platforms), so it is fetched separately from the nari runtime.
install_npkg() {
    ver="$NPKG_VERSION"
    if [ -z "$ver" ]; then
        ver=$(resolve_latest "$NPKG_REPO") || ver=""
        case "$ver" in
            ""|releases) err "could not resolve latest npkg release; skipping npkg"; return 1 ;;
        esac
    fi
    case "$ver" in v*) ;; *) ver="v${ver}" ;; esac

    asset="npkg-${ver}.tar.gz"
    url="https://github.com/${NPKG_REPO}/releases/download/${ver}"

    log "Downloading npkg ${ver}"
    download "${url}/${asset}" "$tmpdir/$asset" \
        || { err "npkg download failed: ${url}/${asset}; skipping npkg"; return 1; }

    if download "${url}/${asset}.sha256" "$tmpdir/${asset}.sha256" 2>/dev/null; then
        expected=$(awk '{print $1; exit}' "$tmpdir/${asset}.sha256")
        actual=$(sha256_of "$tmpdir/$asset")
        [ "$actual" = "$expected" ] || die "npkg checksum mismatch (expected $expected, got $actual)"
        log "npkg checksum OK"
    else
        err "no npkg checksum published; skipping verification"
    fi

    tar -xzf "$tmpdir/$asset" -C "$tmpdir"
    nsrc="$tmpdir/npkg-${ver}"
    [ -f "$nsrc/bin/npkg" ] && [ -d "$nsrc/lib/npkg" ] \
        || { err "unexpected npkg tarball layout; skipping npkg"; return 1; }

    rm -rf "$NARI_HOME/lib/npkg"
    mkdir -p "$NARI_HOME/lib/npkg"
    cp -R "$nsrc/lib/npkg/." "$NARI_HOME/lib/npkg/"
    cp "$nsrc/bin/npkg" "$NARI_HOME/bin/npkg"
    chmod +x "$NARI_HOME/bin/npkg"
    return 0
}

if [ -z "$NPKG_CHOICE" ]; then
    # -r /dev/tty is not enough: the node exists even without a controlling
    # terminal, so test that it actually opens
    if (exec </dev/tty) 2>/dev/null; then
        printf 'Install npkg (the Nari package manager)? [Y/n] ' >/dev/tty
        read -r ans </dev/tty || ans=""
        case "$ans" in
            n|N|no|NO) NPKG_CHOICE="no" ;;
            *) NPKG_CHOICE="yes" ;;
        esac
    else
        NPKG_CHOICE="no"
    fi
fi

if [ "$NPKG_CHOICE" = "yes" ]; then
    log "Installing npkg"
    install_npkg || NPKG_CHOICE="no"
else
    rm -f "$NARI_HOME/bin/npkg"
    rm -rf "$NARI_HOME/lib/npkg"
fi

add_path_to_rc() {
    rc="$1"
    if [ -f "$rc" ] && grep -Fq "$NARI_HOME/bin" "$rc" 2>/dev/null; then
        return 0
    fi
    {
        printf '\n# Added by Nari installer\n'
        printf 'export PATH="%s/bin:$PATH"\n' "$NARI_HOME"
    } >> "$rc"
    log "Added $NARI_HOME/bin to PATH in $rc"
}

case "$(basename "${SHELL:-/bin/sh}")" in
    bash)
        add_path_to_rc "$HOME/.bashrc"
        [ -f "$HOME/.bash_profile" ] && add_path_to_rc "$HOME/.bash_profile"
        ;;
    zsh)
        add_path_to_rc "${ZDOTDIR:-$HOME}/.zshrc"
        ;;
    fish)
        fish_conf="${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d"
        if [ ! -f "$fish_conf/nari.fish" ]; then
            mkdir -p "$fish_conf"
            printf 'fish_add_path --global "%s/bin"\n' "$NARI_HOME" > "$fish_conf/nari.fish"
            log "Added $NARI_HOME/bin to PATH in $fish_conf/nari.fish"
        fi
        ;;
    sh|dash|ash)
        add_path_to_rc "$HOME/.profile"
        ;;
    *)
        log "Add this to your shell's startup file:"
        log "  export PATH=\"$NARI_HOME/bin:\$PATH\""
        ;;
esac

log ""
log "Nari ${VERSION} installed to ${NARI_HOME}"
if [ "$NPKG_CHOICE" = "yes" ]; then
    log "npkg installed: try 'npkg search <query>' once your shell reloads"
fi
log "Open a new terminal (or source your shell rc) and run: nari --help"
