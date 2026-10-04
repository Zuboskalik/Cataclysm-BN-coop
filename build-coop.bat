@echo off
rem Local helper: configure (optional) and build the Windows tiles release.
rem Usage: build-coop.bat [configure]
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
rem vcpkg is expected in src\vcpkg next to this repository unless VCPKG_ROOT is set.
for %%I in ("%~dp0..") do set PROJECT_DIR=%%~fI
if "%VCPKG_ROOT%"=="" set VCPKG_ROOT=%PROJECT_DIR%\src\vcpkg
cd /d %~dp0
if "%1"=="configure" (
  if exist CMakeUserPresets.json del CMakeUserPresets.json
  cmake --preset windows-tiles-sounds-x64-msvc -DTESTS=OFF -DJSON_FORMAT=OFF > out-configure.log 2>&1
  echo configure exit=%ERRORLEVEL%
)
if exist out\build\windows-tiles-sounds-x64-msvc\src\Release\cataclysm-bn-tiles.pdb del out\build\windows-tiles-sounds-x64-msvc\src\Release\cataclysm-bn-tiles.pdb
cmake --build out\build\windows-tiles-sounds-x64-msvc --config Release --target cataclysm-bn-tiles -- /m:2 /v:minimal /nologo > out-build.log 2>&1
echo build exit=%ERRORLEVEL%
