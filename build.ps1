# build.ps1 - Windows build script mirroring build.sh
# Usage: .\build.ps1 [-Release] [-Reconfigure] [-Wipe] [-ClangCl] [-Emscripten] [-DisableLsp]

param(
  [switch]$Release,
  [switch]$Reconfigure,
  [switch]$Wipe,
  [switch]$ClangCl,
  [switch]$Emscripten
)

# ---------------------------------------------------------------------------
# Activate the VS x64 environment so clang-cl / MSVC find the right 64-bit
# MSVC libs (LIB, INCLUDE, PATH, etc. all need to point to x64 variants).
# Without this, clang-cl defaults to x86 libs regardless of --target.
# Skip for Emscripten which manages its own environment.
# ---------------------------------------------------------------------------
function Invoke-VsVars64 {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) {
  Write-Warning 'vswhere.exe not found; skipping VS environment activation. If you see x86 linker errors, run from an x64 Native Tools Command Prompt instead.'
  return
  }
  $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
  if (-not $vsPath) {
  Write-Warning 'No VS installation with MSVC tools found; skipping VS environment activation.'
  return
  }
  $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'
  if (-not (Test-Path $vcvars)) {
  Write-Warning "vcvarsall.bat not found at $vcvars; skipping."
  return
  }

  Write-Host "Activating VS x64 environment from: $vsPath"

  # Run vcvarsall in cmd, dump the resulting environment, then import it.
  $envDump = cmd.exe /c "`"$vcvars`" amd64 >NUL 2>&1 && set"
  foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') {
  [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
  }
  }
}

if (-not $Emscripten) {
  Invoke-VsVars64
}

$BuildType  = 'debug'
$NativeFile  = $null
$MesonOpts  = @()
$ExtraArgs  = @()

if ($Release) {
  Write-Host 'Building as release...'
  $BuildType = 'release'
  $ExtraArgs += '-Db_lto=true'
}

if ($Wipe) {
  Write-Host 'Wiping build directory...'
  $Reconfigure = $true
}

if ($ClangCl) {
  Write-Host 'Using clang-cl toolchain...'
  $NativeFile = 'toolchain\windows-clang-cl.txt'
}

if ($Emscripten) {
  Write-Host 'Building for WebAssembly with Emscripten...'
  $NativeFile  = 'toolchain\emscripten.txt'
  $MesonOpts  += '-Ddisable_ffi=true', '-Ddisable_http=true', '-Ddisable_jit=true'
  # Emscripten builds always use release for performance
  $BuildType  = 'release'
  $ExtraArgs  = $ExtraArgs | Where-Object { $_ -ne '-Db_lto=true' }
  $ExtraArgs  += '-Db_lto=false'
}

# Determine build directory
if ($Emscripten) {
  $BuildDir = "emscripten-$BuildType"
} else {
  $BuildDir = $BuildType
}

$BuildPath = "build\$BuildDir"

if (-not (Test-Path $BuildPath)) {
  $Reconfigure = $true
}

# Run conan, but only if not using emscripten
if (-not $Emscripten) {
  Write-Host 'Installing dependencies via conan...'
  New-Item -ItemType Directory -Force -Path 'build\conan' | Out-Null
  conan install . --output-folder=build\conan --deployer=direct_deploy --deployer-folder=build\conan --build=missing
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Reconfigure) {
  $SetupArgs = @($BuildPath, "--buildtype=$BuildType", '--native-file', 'build\conan\conan_meson_native.ini')

  if ($NativeFile) {
  $SetupArgs += "--native-file", $NativeFile
  }

  if ($ExtraArgs.Count -gt 0) {
  $SetupArgs += $ExtraArgs
  }
  if ($MesonOpts.Count -gt 0) {
  $SetupArgs += $MesonOpts
  }
  if ($Wipe) {
  $SetupArgs = @('--wipe') + $SetupArgs
  }

  & meson setup @SetupArgs
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

meson compile -C $BuildPath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Post-process the Wasm binary with wasm-opt if available
if ($Emscripten) {
  $WasmFile = "$BuildPath\nari.wasm"
  if (Get-Command wasm-opt -ErrorAction SilentlyContinue) {
  Write-Host "Running wasm-opt on $WasmFile..."
  wasm-opt --generate-global-effects --monomorphize --pass-arg=monomorphize-min-benefit@75 -O4 --enable-bulk-memory --enable-sign-ext --gufa --closed-world -O4 --strip-toolchain-annotations --flatten --rereloop -O4 -O4 -o $WasmFile $WasmFile
  } else {
  Write-Host 'wasm-opt not found; skipping (install binaryen to enable)'
  }
}
