@echo off
setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0"

set "BUILD_TYPE=debug"
set "RECONFIGURE=false"
set "WIPE=false"
set "CLANGCL=false"
set "WIN7_COMPAT=false"
set "STATIC_CRT=false"
set "TOOLCHAIN_ROOT=%~dp0build\msvc"
set "PYTHON_EXE=C:\python312\python.exe"
set "CONAN_ROOT=build\wine-conan"
set "BUILD_DIR=build\wine-debug"

:parse_args
if "%~1"=="" goto args_done

if /I "%~1"=="--release" (
  set "BUILD_TYPE=release"
  shift
  goto parse_args
)

if /I "%~1"=="--reconfigure" (
  set "RECONFIGURE=true"
  shift
  goto parse_args
)

if /I "%~1"=="--wipe" (
  set "WIPE=true"
  set "RECONFIGURE=true"
  shift
  goto parse_args
)

if /I "%~1"=="--clang-cl" (
  set "CLANGCL=true"
  shift
  goto parse_args
)

if /I "%~1"=="--win7" (
  set "WIN7_COMPAT=true"
  shift
  goto parse_args
)

if /I "%~1"=="--static-crt" (
  set "STATIC_CRT=true"
  shift
  goto parse_args
)

if /I "%~1"=="--toolchain-root" (
  if "%~2"=="" (
  echo ERROR: --toolchain-root requires a path.
  exit /b 2
  )
  set "TOOLCHAIN_ROOT=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--python-exe" (
  if "%~2"=="" (
  echo ERROR: --python-exe requires a path.
  exit /b 2
  )
  set "PYTHON_EXE=%~2"
  shift
  shift
  goto parse_args
)

echo ERROR: unknown argument %~1
exit /b 2

:args_done
if not exist "%TOOLCHAIN_ROOT%\setup_x64.bat" (
  echo ERROR: portable MSVC toolchain not found at "%TOOLCHAIN_ROOT%".
  echo Expected "%TOOLCHAIN_ROOT%\setup_x64.bat".
  exit /b 1
)

call "%TOOLCHAIN_ROOT%\setup_x64.bat"
if errorlevel 1 exit /b %errorlevel%

echo Active MSVC toolchain: %VCToolsVersion%
echo Active Windows SDK: %WindowsSDKVersion%

set "PYTHONUTF8=1"
set "PIP_DISABLE_PIP_VERSION_CHECK=1"

if not exist "%PYTHON_EXE%" (
  echo ERROR: Windows Python not found at "%PYTHON_EXE%".
  echo Re-run build_wine.sh to bootstrap the Wine prefix.
  exit /b 1
)

for %%I in ("%PYTHON_EXE%") do set "PYTHON_DIR=%%~dpI"
set "PATH=%PYTHON_DIR%;%PYTHON_DIR%Scripts;%PATH%"

"%PYTHON_EXE%" -m pip install --upgrade pip
if errorlevel 1 exit /b %errorlevel%

"%PYTHON_EXE%" -m pip install meson ninja conan cmake
if errorlevel 1 exit /b %errorlevel%

if /I "%BUILD_TYPE%"=="release" set "BUILD_DIR=build\wine-release"
if /I "%WIN7_COMPAT%"=="true" (
  if /I "%BUILD_TYPE%"=="release" (
  set "BUILD_DIR=build\wine-win7-release"
  ) else (
  set "BUILD_DIR=build\wine-win7-debug"
  )
)
if /I "%CLANGCL%"=="true" (
  if /I "%WIN7_COMPAT%"=="true" (
  if /I "%BUILD_TYPE%"=="release" (
  set "BUILD_DIR=build\wine-clangcl-win7-release"
  ) else (
  set "BUILD_DIR=build\wine-clangcl-win7-debug"
  )
  ) else (
  if /I "%BUILD_TYPE%"=="release" (
  set "BUILD_DIR=build\wine-clangcl-release"
  ) else (
  set "BUILD_DIR=build\wine-clangcl-debug"
  )
  )
)

if not exist "%CONAN_ROOT%" mkdir "%CONAN_ROOT%"
set "CONAN_PROFILE=%CONAN_ROOT%\windows_msvc_profile"
set "MESON_CONAN_NATIVE=%CONAN_ROOT%\meson_wine_conan.ini"
set "CONAN_BUILD_TYPE=Debug"
if /I "%BUILD_TYPE%"=="release" set "CONAN_BUILD_TYPE=Release"

if not defined VCToolsVersion (
  echo ERROR: VCToolsVersion is not set after activating the MSVC environment.
  exit /b 1
)

for /f "tokens=1-3 delims=." %%A in ("%VCToolsVersion%") do (
  set "MSVC_MAJOR=%%A"
  set "MSVC_MINOR=%%B"
)

set "CONAN_COMPILER_VERSION=190"
if %MSVC_MINOR% GEQ 10 set "CONAN_COMPILER_VERSION=191"
if %MSVC_MINOR% GEQ 20 set "CONAN_COMPILER_VERSION=192"
if %MSVC_MINOR% GEQ 30 set "CONAN_COMPILER_VERSION=193"
if %MSVC_MINOR% GEQ 50 set "CONAN_COMPILER_VERSION=194"

(
  echo [settings]
  echo arch=x86_64
  echo build_type=!CONAN_BUILD_TYPE!
  echo compiler=msvc
  echo compiler.cppstd=20
  if /I "%STATIC_CRT%"=="true" (
  echo compiler.runtime=static
  ) else (
  echo compiler.runtime=dynamic
  )
  echo compiler.runtime_type=!CONAN_BUILD_TYPE!
  echo compiler.version=!CONAN_COMPILER_VERSION!
  echo os=Windows
  echo [conf]
  echo tools.cmake.cmaketoolchain:generator=Ninja
  echo tools.cmake.cmaketoolchain:extra_variables={'BUILD_LIBCURL_DOCS': {'value': False, 'cache': True, 'type': 'BOOL', 'force': True}, 'BUILD_MISC_DOCS': {'value': False, 'cache': True, 'type': 'BOOL', 'force': True}, 'ENABLE_CURL_MANUAL': {'value': False, 'cache': True, 'type': 'BOOL', 'force': True}}
  echo tools.microsoft.msbuild:installation_path=
) > "%CONAN_PROFILE%"
if errorlevel 1 exit /b %errorlevel%

echo Using Conan profile:
type "%CONAN_PROFILE%"
if errorlevel 1 exit /b %errorlevel%

(
  echo [properties]
  echo conan_deploy_dir = 'build/wine-conan/direct_deploy'
) > "%MESON_CONAN_NATIVE%"
if errorlevel 1 exit /b %errorlevel%

echo Installing dependencies via Conan...
conan install . -pr:h="%CONAN_PROFILE%" -pr:b="%CONAN_PROFILE%" --output-folder="%CONAN_ROOT%" --deployer=direct_deploy --deployer-folder="%CONAN_ROOT%" --build=missing
if errorlevel 1 exit /b %errorlevel%

if not exist "%BUILD_DIR%" set "RECONFIGURE=true"

if "%RECONFIGURE%"=="true" (
  set "MESON_SETUP_ARGS=%BUILD_DIR% --buildtype=%BUILD_TYPE% --native-file %CONAN_ROOT%\conan_meson_native.ini --native-file %MESON_CONAN_NATIVE%"
  if "%BUILD_TYPE%"=="release" set "MESON_SETUP_ARGS=!MESON_SETUP_ARGS! -Db_lto=true"
  if "%CLANGCL%"=="true" set "MESON_SETUP_ARGS=!MESON_SETUP_ARGS! --native-file toolchain\windows-clang-cl.txt"
  if "%WIN7_COMPAT%"=="true" set "MESON_SETUP_ARGS=!MESON_SETUP_ARGS! -Dwindows_win7_compat=true"
  if "%STATIC_CRT%"=="true" set "MESON_SETUP_ARGS=!MESON_SETUP_ARGS! -Db_vscrt=static_from_buildtype"
  if "%WIPE%"=="true" set "MESON_SETUP_ARGS=--wipe !MESON_SETUP_ARGS!"

  echo Configuring Meson...
  call "%PYTHON_EXE%" -m mesonbuild.mesonmain setup !MESON_SETUP_ARGS!
  if errorlevel 1 exit /b %errorlevel%
)

echo Building...
"%PYTHON_EXE%" -m mesonbuild.mesonmain compile -C "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%

if /I not "%STATIC_CRT%"=="true" (
  set "RUNTIME_BIN_DIR=%TOOLCHAIN_ROOT%\VC\Tools\MSVC\%VCToolsVersion%\bin\Hostx64\x64"

  echo Staging MSVC runtime DLLs into %BUILD_DIR%...
  for %%F in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do (
  if exist "%RUNTIME_BIN_DIR%\%%F" (
    copy /Y "%RUNTIME_BIN_DIR%\%%F" "%BUILD_DIR%\%%F" >nul
    if errorlevel 1 exit /b %errorlevel%
  )
  )
)
