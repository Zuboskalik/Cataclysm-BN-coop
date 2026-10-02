@echo off
rem Local helper: packs the Release build into a zip like the official CI does.
rem Usage: package-coop.bat <version-label>   e.g. package-coop.bat coop-v0.1.0
if "%1"=="" (
  echo Usage: package-coop.bat ^<version-label^>
  exit /b 1
)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cd /d %~dp0
set DIST=%~dp0out\dist\cataclysmbn-%1
if exist "%DIST%" rmdir /s /q "%DIST%"
if not exist lang\mo mkdir lang\mo
cmake --install out\build\windows-tiles-sounds-x64-msvc --prefix "%DIST%" --config Release
if errorlevel 1 exit /b 1
powershell -NoProfile -Command "Compress-Archive -Force -Path '%DIST%\*' -DestinationPath '%~dp0out\dist\cbn-windows-tiles-x64-msvc-%1.zip'"
echo package exit=%ERRORLEVEL%
