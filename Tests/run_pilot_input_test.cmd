@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
pushd "%~dp0..\.codex-temp-dia2dump"
cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN "%~dp0pilot_input_test.cpp" /Fepilot_input_test.exe /Fopilot_input_test.obj /link user32.lib
if errorlevel 1 exit /b 1
pilot_input_test.exe
set result=%errorlevel%
popd
exit /b %result%
