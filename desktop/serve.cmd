@echo off
rem serve.cmd -- double-clickable wrapper around serve.ps1
rem
rem Kept ASCII-only on purpose: .cmd files are read using the console's OEM
rem codepage (936 here), so non-ASCII text in a batch file renders as garbage
rem or breaks parsing. All Chinese output lives in serve.ps1, which PowerShell
rem reads as UTF-8.
rem
rem Serving over http://localhost is required because Web Bluetooth / Web Serial
rem are only exposed in a secure context -- opening index.html via file:// gives
rem you no navigator.bluetooth at all. See serve.ps1 for details.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0serve.ps1" %*
if errorlevel 1 pause
