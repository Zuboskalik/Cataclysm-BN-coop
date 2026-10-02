@echo off
rem Local helper: packs the Release build into a zip like the official CI does.
rem Usage: package-coop.bat <version-label> [official-bn-folder]
rem   e.g. package-coop.bat coop-v0.1.0
rem        package-coop.bat coop-v0.1.0 C:\games\cbn-windows-tiles-x64-msvc-v0.13.0
rem Compiled translations (lang\mo) come from Transifex in the official CI and are
rem not in the repository, so they are copied from an official BN release folder.
rem Strings added by the co-op port stay in English.
if "%1"=="" (
  echo Usage: package-coop.bat ^<version-label^> [official-bn-folder]
  exit /b 1
)
set OFFICIAL=%~2
if "%OFFICIAL%"=="" set OFFICIAL=C:\games\cbn-windows-tiles-x64-msvc-v0.13.0
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cd /d %~dp0
set DIST=%~dp0out\dist\cataclysmbn-%1
if exist "%DIST%" rmdir /s /q "%DIST%"
if not exist lang\mo mkdir lang\mo
cmake --install out\build\windows-tiles-sounds-x64-msvc --prefix "%DIST%" --config Release
if errorlevel 1 exit /b 1
if exist "%OFFICIAL%\lang\mo" (
  echo Copying translations from %OFFICIAL%\lang\mo
  robocopy "%OFFICIAL%\lang\mo" "%DIST%\lang\mo" /E /NFL /NDL /NJH /NJS /NP >nul
) else (
  echo WARNING: %OFFICIAL%\lang\mo not found - the package will be English only.
)
powershell -NoProfile -Command "Compress-Archive -Force -Path '%DIST%\*' -DestinationPath '%~dp0out\dist\cbn-windows-tiles-x64-msvc-%1.zip'"
echo package exit=%ERRORLEVEL%
