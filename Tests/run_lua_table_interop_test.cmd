@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if not "%errorlevel%"=="0" exit /b 1
powershell.exe -NoProfile -File "%~dp0run_lua_table_interop_test.ps1"
exit /b %errorlevel%
