@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
pushd "%~dp0..\.codex-temp-dia2dump"
cl /nologo /std:c++20 /EHsc /MT /Gy /O2 /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS "%~dp0pilot_log_test.cpp" /Fepilot_log_test.exe /Fopilot_log_test.obj /link /LTCG /OPT:REF "%~dp0..\obj\DarkFlameClient\Release\x86\imgui.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_draw.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_tables.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_widgets.obj" user32.lib imm32.lib gdi32.lib
if errorlevel 1 exit /b 1
pilot_log_test.exe
set result=%errorlevel%
popd
exit /b %result%
