#!/bin/sh
# Nari uninstaller.
#   curl -fsSL https://raw.githubusercontent.com/wearrrrr/Nari/main/uninstall.sh | sh
set -eu

NARI_HOME="${NARI_HOME:-$HOME/.nari}"
PURGE=0
ASSUME_YES=0
QUIET=0

usage() {
    cat <<EOF
Nari uninstaller

Usage:
  uninstall.sh [--prefix <dir>] [--purge] [--yes] [--quiet] [--help]

Options:
  --prefix <dir>    Toolchain install dir to remove (default: \$NARI_HOME, ~/.nari).
  --purge           Also delete the npkg package store, cache, and saved
                    credentials under ~/.nari (store, cache/npkg, npkg).
  --yes             Do not prompt for confirmation.
  --quiet           Suppress non-error output.
  --help            Show this message.

Environment:
  NARI_HOME         Same as --prefix.
EOF
}

log() { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }
err() { printf 'uninstall.sh: %s\n' "$*" >&2; }
die() { err "$*"; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) NARI_HOME="$2"; shift 2 ;;
        --prefix=*) NARI_HOME="${1#*=}"; shift ;;
        --purge) PURGE=1; shift ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        --quiet) QUIET=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) err "unknown option: $1"; usage >&2; exit 2 ;;
    esac
done

# collect the toolchain artifacts install.sh created under the prefix.
targets=""
for f in bin/nari bin/naric bin/npkg lib/npkg; do
    [ -e "$NARI_HOME/$f" ] && targets="$targets $NARI_HOME/$f"
done

# npkg's runtime data always lives under ~/.nari, independent of the toolchain
# prefix. Detect it up front so we can offer to remove it even without --purge.
data_present=0
for d in "$HOME/.nari/store" "$HOME/.nari/cache/npkg" "$HOME/.nari/npkg"; do
    [ -e "$d" ] && data_present=1
done

interactive=0
if (exec </dev/tty) 2>/dev/null; then interactive=1; fi

# So users don't have to know --purge exists, offer the data removal inline
# rather than leaving it stranded for a flag they may never discover.
if [ "$PURGE" -eq 0 ] && [ "$data_present" -eq 1 ] && [ "$ASSUME_YES" -eq 0 ] && [ "$interactive" -eq 1 ]; then
    printf 'Also remove the npkg package store and saved credentials under ~/.nari? [y/N] ' >/dev/tty
    read -r ans </dev/tty || ans=""
    case "$ans" in y|Y|yes|YES) PURGE=1 ;; esac
fi

data_dirs=""
if [ "$PURGE" -eq 1 ]; then
    for d in "$HOME/.nari/store" "$HOME/.nari/cache/npkg" "$HOME/.nari/npkg"; do
        [ -e "$d" ] && data_dirs="$data_dirs $d"
    done
fi

if [ -z "$targets" ] && [ -z "$data_dirs" ]; then
    log "Nothing to remove (no Nari install found under $NARI_HOME)."
    exit 0
fi

log "The following will be removed:"
for t in $targets $data_dirs; do log "  $t"; done
log "  PATH entries added by the installer (shell rc files)"

# Confirm when attached to a terminal, unless --yes was given.
if [ "$ASSUME_YES" -eq 0 ] && [ "$interactive" -eq 1 ]; then
    printf 'Proceed? [y/N] ' >/dev/tty
    read -r ans </dev/tty || ans=""
    case "$ans" in
        y|Y|yes|YES) ;;
        *) die "aborted" ;;
    esac
fi

for t in $targets; do rm -rf "$t"; done
for d in $data_dirs; do rm -rf "$d"; done

# reverse add_path_to_rc
clean_rc() {
    rc="$1"
    [ -f "$rc" ] || return 0
    grep -Fq "# Added by Nari installer" "$rc" 2>/dev/null || return 0
    tmp="${rc}.nari-uninstall.$$"
    awk '
        /^# Added by Nari installer$/ { skip=1; next }
        skip>0 { skip--; next }
        { print }
    ' "$rc" > "$tmp" && cat "$tmp" > "$rc"
    rm -f "$tmp"
    log "Removed PATH entry from $rc"
}

clean_rc "$HOME/.bashrc"
clean_rc "$HOME/.bash_profile"
clean_rc "${ZDOTDIR:-$HOME}/.zshrc"
clean_rc "$HOME/.profile"

fish_conf="${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/nari.fish"
if [ -f "$fish_conf" ]; then
    rm -f "$fish_conf"
    log "Removed $fish_conf"
fi

# drop now-empty install dirs, but never remove a non-empty prefix.
rmdir "$NARI_HOME/bin" "$NARI_HOME/lib" 2>/dev/null || true
rmdir "$NARI_HOME" 2>/dev/null || true
if [ "$PURGE" -eq 1 ]; then
    rmdir "$HOME/.nari/cache" 2>/dev/null || true
    rmdir "$HOME/.nari" 2>/dev/null || true
fi

log ""
log "Nari uninstalled."
if [ "$PURGE" -eq 0 ] && [ -d "$HOME/.nari" ]; then
    log "The npkg package store and credentials under ~/.nari were kept."
    log "Run with --purge to remove them too."
fi
log "Open a new terminal for the PATH change to take effect."
