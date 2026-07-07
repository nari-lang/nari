#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT="$SCRIPT_DIR"

WINE_PREFIX="${PROJECT_ROOT}/build/wine-prefix"
TOOLCHAIN_ROOT="${PROJECT_ROOT}/build/msvc"
TOOLCHAIN_DOWNLOADS="${PROJECT_ROOT}/build/msvc-downloads"
WINE_DOWNLOADS="${PROJECT_ROOT}/build/wine-downloads"
PYTHON_VERSION="3.12.10"
PYTHON_ROOT="${WINE_PREFIX}/drive_c/python312"
PYTHON_ROOT_EXPLICIT=false
RECREATE_PREFIX=false
SKIP_PYTHON_INSTALL=false
SKIP_TOOLCHAIN_INSTALL=false
WIN7_COMPAT=false
STATIC_CRT=false
TOOLSET=""
VS_VERSION=""
BUILD_ARGS=()

usage() {
  cat <<'EOF'
Usage: ./build_wine.sh [options]

Options:
  --release  Build release instead of debug.
  --reconfigure  Force Meson reconfigure.
  --wipe  Wipe the Meson build dir before configuring.
  --clang-cl  Use clang-cl instead of cl.exe.
  --win7  Enable the Windows 7 compatibility build mode.
  --static-crt  Use the static MSVC runtime (/MT or /MTd).
  --toolset NAME  Toolset family for install_msvc.py (`v143`, `v142`, `latest`).
  --vs VER  Visual Studio manifest line for install_msvc.py (`2022`, `2019`, `latest`, `2026`).
  --prefix PATH  Wine prefix to create/use.
  --toolchain-root PATH  Portable MSVC/SDK directory.
  --downloads PATH  Download cache for Python + MSVC assets.
  --python-version VER  Windows Python version to install in Wine.
  --python-root PATH  Location inside the prefix for portable Windows Python.
  --msvc-version VER  Specific MSVC version for install_msvc.py.
  --sdk-version VER  Specific Windows SDK version for install_msvc.py.
  --recreate-prefix  Delete and recreate the Wine prefix first.
  --skip-python-install  Assume Python is already installed in the prefix.
  --skip-toolchain-install
  Assume the portable MSVC toolchain already exists.
  --help  Show this help.
EOF
}

MSVC_VERSION=""
SDK_VERSION=""

while [[ $# -gt 0 ]]; do
  case "$1" in
  --release|--reconfigure|--wipe|--clang-cl|--win7|--static-crt)
  if [[ "$1" == "--win7" ]]; then
  WIN7_COMPAT=true
  fi
  if [[ "$1" == "--static-crt" ]]; then
  STATIC_CRT=true
  fi
  BUILD_ARGS+=("$1")
  shift
  ;;
  --prefix)
  WINE_PREFIX="$2"
  shift 2
  ;;
  --toolchain-root)
  TOOLCHAIN_ROOT="$2"
  shift 2
  ;;
  --downloads)
  TOOLCHAIN_DOWNLOADS="$2/msvc"
  WINE_DOWNLOADS="$2/wine"
  shift 2
  ;;
  --python-version)
  PYTHON_VERSION="$2"
  shift 2
  ;;
  --python-root)
  PYTHON_ROOT="$2"
  PYTHON_ROOT_EXPLICIT=true
  shift 2
  ;;
  --msvc-version)
  MSVC_VERSION="$2"
  shift 2
  ;;
  --toolset)
  TOOLSET="$2"
  shift 2
  ;;
  --sdk-version)
  SDK_VERSION="$2"
  shift 2
  ;;
  --vs)
  VS_VERSION="$2"
  shift 2
  ;;
  --recreate-prefix)
  RECREATE_PREFIX=true
  shift
  ;;
  --skip-python-install)
  SKIP_PYTHON_INSTALL=true
  shift
  ;;
  --skip-toolchain-install)
  SKIP_TOOLCHAIN_INSTALL=true
  shift
  ;;
  --help|-h)
  usage
  exit 0
  ;;
  *)
  echo "Unknown argument: $1" >&2
  usage >&2
  exit 2
  ;;
  esac
done

if [[ "$PYTHON_ROOT_EXPLICIT" != true ]]; then
  PYTHON_ROOT="${WINE_PREFIX}/drive_c/python312"
fi

if [[ "$WIN7_COMPAT" == true ]]; then
  if [[ -z "$VS_VERSION" ]]; then
  VS_VERSION="2022"
  fi
  if [[ -z "$MSVC_VERSION" && -z "$TOOLSET" ]]; then
  MSVC_VERSION="14.36"
  fi
fi

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
  echo "Missing required command: $1" >&2
  exit 1
  }
}

require_cmd wine
require_cmd wineboot
require_cmd wineserver
require_cmd winepath
require_cmd curl
require_cmd python3
require_cmd unzip

mkdir -p "$TOOLCHAIN_DOWNLOADS" "$WINE_DOWNLOADS"

export WINEPREFIX="$WINE_PREFIX"
export WINEARCH=win64
export WINEDEBUG=-all

if [[ "$RECREATE_PREFIX" == true && -d "$WINE_PREFIX" ]]; then
  rm -rf "$WINE_PREFIX"
fi

if [[ ! -d "$WINE_PREFIX" ]]; then
  echo "Creating Wine prefix at $WINE_PREFIX"
  mkdir -p "$WINE_PREFIX"
  wineboot -u
  wineserver -w
fi

ensure_windows_python() {
  if [[ "$SKIP_PYTHON_INSTALL" == true ]]; then
  return
  fi

  local python_exe_host="${PYTHON_ROOT}/python.exe"
  local python_pkg="python-${PYTHON_VERSION}.nupkg"
  local python_pkg_path="${WINE_DOWNLOADS}/${python_pkg}"
  local python_pkg_url="https://www.nuget.org/api/v2/package/python/${PYTHON_VERSION}"
  local unpack_dir

  if [[ -f "$python_exe_host" ]]; then
  return
  fi

  if [[ ! -f "$python_pkg_path" ]]; then
  echo "Downloading portable Windows Python ${PYTHON_VERSION}"
  curl -fL "$python_pkg_url" -o "$python_pkg_path"
  fi

  echo "Unpacking Windows Python into the Wine prefix"
  rm -rf "$PYTHON_ROOT"
  mkdir -p "$PYTHON_ROOT"
  unpack_dir=$(mktemp -d)
  unzip -q -o "$python_pkg_path" -d "$unpack_dir"

  if [[ -d "${unpack_dir}/tools" ]]; then
  cp -a "${unpack_dir}/tools/." "$PYTHON_ROOT/"
  else
  cp -a "${unpack_dir}/." "$PYTHON_ROOT/"
  fi
  rm -rf "$unpack_dir"

  local python_exe_win
  python_exe_win=$(winepath -w "$python_exe_host")

  echo "Bootstrapping pip inside portable Windows Python"
  wine "$python_exe_win" -m ensurepip --upgrade
  wineserver -w
  wine "$python_exe_win" -m pip --version >/dev/null
}

ensure_portable_msvc() {
  if [[ "$SKIP_TOOLCHAIN_INSTALL" == true ]]; then
  return
  fi

  local toolchain_selection_explicit=false
  if [[ -n "$MSVC_VERSION" || -n "$SDK_VERSION" || -n "$TOOLSET" || -n "$VS_VERSION" ]]; then
  toolchain_selection_explicit=true
  fi

  if [[ -f "${TOOLCHAIN_ROOT}/setup_x64.bat" && "$toolchain_selection_explicit" != true ]]; then
  return
  fi

  if [[ -d "${TOOLCHAIN_ROOT}" && "$toolchain_selection_explicit" == true ]]; then
  echo "Recreating portable MSVC toolchain at ${TOOLCHAIN_ROOT}"
  rm -rf "${TOOLCHAIN_ROOT}"
  fi

  echo "Downloading portable MSVC + Windows SDK into ${TOOLCHAIN_ROOT}"

  local install_args=(
  "${PROJECT_ROOT}/install_msvc.py"
  --accept-license
  --output "${TOOLCHAIN_ROOT}"
  --downloads "${TOOLCHAIN_DOWNLOADS}"
  )

  if [[ -n "$MSVC_VERSION" ]]; then
  install_args+=(--msvc-version "$MSVC_VERSION")
  fi

  if [[ -n "$TOOLSET" ]]; then
  install_args+=(--toolset "$TOOLSET")
  fi

  if [[ -n "$SDK_VERSION" ]]; then
  install_args+=(--sdk-version "$SDK_VERSION")
  fi

  if [[ -n "$VS_VERSION" ]]; then
  install_args+=(--vs "$VS_VERSION")
  fi

  python3 "${install_args[@]}"
}

ensure_windows_python
ensure_portable_msvc

cmd_path_win=$(winepath -w "${PROJECT_ROOT}/build_wine.cmd")
toolchain_root_win=$(winepath -w "${TOOLCHAIN_ROOT}")
python_exe_win=$(winepath -w "${PYTHON_ROOT}/python.exe")

echo "Building Windows target inside Wine"
wine cmd /c call "$cmd_path_win" --toolchain-root "$toolchain_root_win" --python-exe "$python_exe_win" "${BUILD_ARGS[@]}"
