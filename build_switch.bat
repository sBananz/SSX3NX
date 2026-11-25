@echo off
rem SSX3NX -- build SSX3NX.nro (SSX 3 for the Nintendo Switch) on Windows.
rem   build_switch.bat
rem Needs MSYS2 (UCRT64 Python with capstone, for scripts/regen.sh) and Docker
rem Desktop, running. Port of OpenTrickyNX's build_switch.bat (GPL-3.0-only).
setlocal
set "MSYS2=%MSYS2_ROOT%"
if "%MSYS2%"=="" set "MSYS2=C:\msys64"
if not exist "%MSYS2%\usr\bin\bash.exe" (
  echo MSYS2 not found in %MSYS2%.
  echo Install it from https://www.msys2.org/ or set MSYS2_ROOT to its folder.
  pause
  exit /b 1
)
where docker >nul 2>nul
if errorlevel 1 (
  echo Docker not found. Install Docker Desktop and start it: it provides the
  echo Switch compiler ^(the ssx3nx-build image, from devkitpro/devkita64^).
  pause
  exit /b 1
)
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
rem Keep the Windows PATH inside MSYS2, so docker.exe is found there too.
set "MSYS2_PATH_TYPE=inherit"
cd /d "%~dp0"
"%MSYS2%\usr\bin\bash.exe" -lc "./build_switch.sh"
set ERR=%ERRORLEVEL%
if not "%ERR%"=="0" echo Build failed (%ERR%). See the messages above.
pause
exit /b %ERR%
