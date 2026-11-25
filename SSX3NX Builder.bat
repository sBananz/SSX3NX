@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem SSX3NX Builder: double-click to set up the tools, build SSX3NX.nro from
rem your own SSX 3 (PAL Xbox) game and copy it to the SD card (docs/windows-guide.md).
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tools\windows\builder.ps1"
